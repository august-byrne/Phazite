// SPDX-License-Identifier: AGPL-3.0-or-later
// The EQ's editor (src/ui/devices/EqEditor.h), and the spectrum its analyzer shows (src/devices/core/Spectrum.h).
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/core/Spectrum.h"
#include "devices/eq/Equalizer.h"
#include "model/commands/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/EqEditor.h"

#include <cmath>
#include <vector>

namespace
{
using Eq = phazite::devices::Equalizer;

te::Plugin::Ptr addEq (te::Edit& edit)
{
    auto plugin = edit.getPluginCache().createNewPlugin (Eq::typeID, {});
    te::getAudioTracks (edit)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
    return plugin;
}

float valueOf (te::Plugin& plugin, const char* id)
{
    return plugin.getAutomatableParameterByID (id)->getCurrentValue();
}

// The value a knob shows, by its title; NaN without such a knob.
double knobValue (juce::Component& root, const juce::String& title)
{
    for (auto* knob : phazite::test::findAll<phazite::ui::Knob> (root))
        if (knob->getTitle() == title)
            return knob->getValue();

    return std::nan ("");
}

juce::String knobText (juce::Component& root, const juce::String& title, double value)
{
    for (auto* knob : phazite::test::findAll<phazite::ui::Knob> (root))
        if (knob->getTitle() == title)
            return knob->getTextFromValue (value);

    return {};
}
} // namespace

TEST_CASE ("A spectrum shows a sine at its frequency and level, and falls slowly when it stops")
{
    phazite::devices::Spectrum spectrum;
    spectrum.setSampleRate (48000.0);
    std::vector<float> sound (4096);

    // 1.5 kHz falls on a bin: 1500 * 4096 / 48000 = 128.
    for (size_t i = 0; i < sound.size(); ++i)
        sound[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1500.0 * (double) i / 48000.0);

    spectrum.add (sound.data(), (int) sound.size());
    spectrum.update (100.0f);
    CHECK (spectrum.getLevelAt (1500.0) == doctest::Approx (-6.02).epsilon (0.02));
    CHECK (spectrum.getLevelAt (6000.0) < -60.0f);

    std::fill (sound.begin(), sound.end(), 0.0f);
    spectrum.add (sound.data(), (int) sound.size());
    spectrum.update (3.0f);
    CHECK (spectrum.getLevelAt (1500.0) == doctest::Approx (-9.02).epsilon (0.02));
}

TEST_CASE ("The EQ's editor shows the selected band's controls, and its analyzer listens while it is open")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto eq = addEq (*edit);
    const phazite::DeviceHandle device (*eq);
    auto& tap = device.getProcessor<Eq>()->getOutputTap();

    {
        phazite::ui::EqEditor editor (device);
        CHECK (editor.getSelectedBand() == 2);
        CHECK (tap.isListening());
        CHECK (knobValue (editor, "Frequency") == doctest::Approx (250.0));

        editor.selectBand (5);
        CHECK (knobValue (editor, "Frequency") == doctest::Approx (6000.0));
        CHECK (knobText (editor, "Gain", 0.0) == juce::String ("0.0 dB"));

        phazite::test::checkChildrenApart (editor);
    }

    CHECK_FALSE (tap.isListening());
    CHECK (dynamic_cast<phazite::ui::EqEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);
}

TEST_CASE ("Dragging an EQ band's dot moves its frequency and gain as one undo step; a double-click and the wheel work too")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto eq = addEq (*edit);
    phazite::test::settle();
    edit->getUndoManager().clearUndoHistory();

    phazite::ui::EqEditor editor ((phazite::DeviceHandle (*eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    phazite::test::Mouse mouse (editor.getGraph());

    // Band 4: a bell at 800 Hz, flat.
    const auto dot = editor.getDotPosition (3).roundToInt();
    mouse.press (dot);
    mouse.dragTo (dot + juce::Point<int> (60, -40));
    mouse.release();
    CHECK (editor.getSelectedBand() == 3);
    CHECK (valueOf (*eq, "band4Frequency") > 900.0f);
    CHECK (valueOf (*eq, "band4Gain") > 3.0f);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Move Band 4"));

    phazite::undo (*edit);
    editor.refresh();
    CHECK (valueOf (*eq, "band4Frequency") == doctest::Approx (800.0f));
    CHECK (valueOf (*eq, "band4Gain") == doctest::Approx (0.0f));

    // A double-click turns the band off, and on again.
    mouse.pause();
    mouse.doubleClick (editor.getDotPosition (3).roundToInt());
    CHECK (valueOf (*eq, "band4On") == doctest::Approx (0.0f));
    mouse.doubleClick (editor.getDotPosition (3).roundToInt());
    CHECK (valueOf (*eq, "band4On") == doctest::Approx (1.0f));

    // The wheel narrows or widens it.
    mouse.wheel (editor.getDotPosition (3).roundToInt(), 0.5f);
    CHECK (valueOf (*eq, "band4Q") == doctest::Approx (2.0f));
}
