// SPDX-License-Identifier: AGPL-3.0-or-later
// The reverb's editor (src/ui/devices/ReverbEditor.h).
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/reverb/Reverb.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/ReverbEditor.h"

using phazite::devices::Reverb;

TEST_CASE ("The reverb's editor shows a knob for each control, and its graph follows the decay")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (Reverb::typeID, {});
    REQUIRE (phazite::insertDevice (*te::getAudioTracks (*edit)[0], plugin) != nullptr);
    phazite::test::settle();

    const auto device = phazite::makeDeviceHandle (*plugin);
    CHECK (dynamic_cast<phazite::ui::ReverbEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::ReverbEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    CHECK (phazite::test::findAll<phazite::ui::Knob> (editor).size() == Reverb::info.parameters.size() - 1); // its mix is its header's
    phazite::test::checkChildrenApart (editor);

    // A longer decay, as an undo step the model names, and the map shows more time.
    const auto before = editor.getShownSeconds();
    auto* decay = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("decay"));
    REQUIRE (decay != nullptr);
    decay->setValue (8.0, juce::sendNotificationSync);
    editor.refresh();
    CHECK (plugin->getAutomatableParameterByID ("decay")->getCurrentValue() == doctest::Approx (8.0f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Decay"));
    CHECK (editor.getShownSeconds() > 3.0 * before);

    // Dragging the gap at the tail's start to the right lengthens the pre-delay, as one undo step.
    auto& map = editor.getGraph();
    const auto gap = editor.getPredelayHandle().roundToInt();
    phazite::test::Mouse mouse (map);
    mouse.press (gap);
    mouse.dragTo (gap + juce::Point<int> (10, 0));
    mouse.dragTo (gap + juce::Point<int> (20, 0));
    mouse.release();
    CHECK (plugin->getAutomatableParameterByID ("predelay")->getCurrentValue() > 100.0f);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Pre-Delay"));

    // And its end to the left, a shorter decay.
    mouse.pause();
    const auto end = editor.getDecayHandle().roundToInt();
    mouse.press (end);
    mouse.dragTo (end - juce::Point<int> (40, 0));
    mouse.release();
    CHECK (plugin->getAutomatableParameterByID ("decay")->getCurrentValue() < 8.0f);
}
