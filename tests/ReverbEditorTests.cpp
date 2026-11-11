// SPDX-License-Identifier: AGPL-3.0-or-later
// The reverb's editor (src/ui/devices/ReverbEditor.h).
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/reverb/Reverb.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/ReverbEditor.h"

using phazite::devices::Reverb;

TEST_CASE ("The reverb's editor shows a knob for each control, and its graph follows the decay")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = phazite::insertDeviceState (s, song.firstTrack, phazite::makeOwnDeviceState (s, Reverb::typeID));
    REQUIRE (id.isValid());
    phazite::getSession (song.context)->update();
    const auto valueOf = [&s, id] (const char* parameter)
    { return phazite::getValueSetByHand (s, phazite::ParameterRef { id, parameter }); };

    const auto device = phazite::makeDeviceHandle (s, id);
    CHECK (dynamic_cast<phazite::ui::ReverbEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::ReverbEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    // A knob for each, but its mix, which is its header's, and its mode and its high cut's slope, which are lists.
    CHECK (phazite::test::findAll<phazite::ui::Knob> (editor).size() == Reverb::info.parameters.size() - 3);
    CHECK (dynamic_cast<phazite::ui::Knob*> (editor.findControl ("diffusion")) != nullptr);
    phazite::test::checkChildrenApart (editor);

    // Its mode is a list, a room at first; a plate picked is one undo step, and the map says which it shows.
    auto* mode = dynamic_cast<juce::ComboBox*> (editor.findControl ("mode"));
    REQUIRE (mode != nullptr);
    CHECK (mode->getTitle() == juce::String ("Mode"));
    CHECK (mode->getNumItems() == 2);
    CHECK (mode->getText() == juce::String ("Room"));
    CHECK (editor.getModeText() == juce::String ("Room"));
    mode->setSelectedItemIndex (Reverb::plate, juce::sendNotificationSync);
    editor.refresh();
    CHECK (valueOf ("mode") == (float) Reverb::plate);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Mode"));
    CHECK (editor.getModeText() == juce::String ("Plate"));

    // A plate's decay too short for its diffusion and its size: the map says what it holds them to.
    auto* decayKnob = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("decay"));
    auto* diffusion = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("diffusion"));
    REQUIRE (decayKnob != nullptr);
    REQUIRE (diffusion != nullptr);
    diffusion->setValue (1.0, juce::sendNotificationSync);
    decayKnob->setValue (0.5, juce::sendNotificationSync);
    editor.refresh();
    CHECK (editor.getModeText().startsWith ("Plate  Diffusion "));
    CHECK (editor.getModeText().contains ("  Size "));
    const auto shortTail = editor.getShownSeconds();
    decayKnob->setValue (2.0, juce::sendNotificationSync);
    diffusion->setValue (0.5, juce::sendNotificationSync);
    editor.refresh();
    CHECK (editor.getModeText() == juce::String ("Plate"));
    CHECK (editor.getShownSeconds() > 2.0 * shortTail); // the map draws the plate's decay

    // Its high cut's slope is a list too, 12 dB an octave at first.
    auto* slope = dynamic_cast<juce::ComboBox*> (editor.findControl ("highCutSlope"));
    REQUIRE (slope != nullptr);
    CHECK (slope->getTitle() == juce::String ("High Cut Slope"));
    CHECK (slope->getText() == juce::String ("12 dB"));
    slope->setSelectedItemIndex (0, juce::sendNotificationSync);
    CHECK (valueOf ("highCutSlope") == 0.0f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change High Cut Slope"));

    // A longer decay, as an undo step the model names, and the map shows more time.
    const auto before = editor.getShownSeconds();
    auto* decay = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("decay"));
    REQUIRE (decay != nullptr);
    decay->setValue (8.0, juce::sendNotificationSync);
    editor.refresh();
    CHECK (valueOf ("decay") == doctest::Approx (8.0f));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Decay"));
    CHECK (editor.getShownSeconds() > 3.0 * before);

    // Dragging the gap at the tail's start to the right lengthens the pre-delay, as one undo step.
    auto& map = editor.getGraph();
    const auto gap = editor.getPredelayHandle().roundToInt();
    phazite::test::Mouse mouse (map);
    mouse.press (gap);
    mouse.dragTo (gap + juce::Point<int> (10, 0));
    mouse.dragTo (gap + juce::Point<int> (20, 0));
    mouse.release();
    CHECK (valueOf ("predelay") > 100.0f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Pre-Delay"));

    // And its end to the left, a shorter decay.
    mouse.pause();
    const auto end = editor.getDecayHandle().roundToInt();
    mouse.press (end);
    mouse.dragTo (end - juce::Point<int> (40, 0));
    mouse.release();
    CHECK (valueOf ("decay") < 8.0f);
}
