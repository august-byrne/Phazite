// SPDX-License-Identifier: AGPL-3.0-or-later
// The multiband's and the maximizer's editors (src/ui/devices/MultibandEditor.h, MaximizerEditor.h): a control for each
// parameter but the mix (the header's), what their bands show as sound plays, and the drags on their pictures, each one
// undo step.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Maximizer.h"
#include "devices/dynamics/Multiband.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/MaximizerEditor.h"
#include "ui/devices/MultibandEditor.h"

#include <vector>

namespace
{
using phazite::devices::Maximizer;
using phazite::devices::Multiband;

// A device on a track of its own Edit.
struct Song
{
    explicit Song (const char* typeID)
    {
        plugin = edit->getPluginCache().createNewPlugin (typeID, {});
        te::getAudioTracks (*edit)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
        phazite::test::settle();
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { plugin = nullptr; }

    float valueOf (const char* id) const { return plugin->getAutomatableParameterByID (id)->getCurrentValue(); }
    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr plugin;
};

// A second of a low sine at a level through a device's DSP, as the audio thread would play it, with its defaults.
void playLowSine (phazite::devices::Processor& processor, const phazite::devices::DeviceInfo& info, float decibels)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    constexpr int numSamples = 48000;
    juce::AudioBuffer<float> buffer (2, numSamples);
    const auto amplitude = juce::Decibels::decibelsToGain (decibels);

    for (int i = 0; i < numSamples; ++i)
        for (int channel = 0; channel < 2; ++channel)
            buffer.setSample (channel, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * 50.0 * i / 48000.0));

    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = numSamples;
    block.parameters = values;
    processor.prepare (48000.0, numSamples);
    processor.process (block);
}

void checkEveryParameterButMixHasAControl (const phazite::ui::DeviceEditorBase& editor, const phazite::devices::DeviceInfo& info)
{
    for (const auto& parameter : info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        CHECK ((editor.findControl (parameter.id) != nullptr) == (parameter.id != phazite::devices::mixParameterID));
    }
}
} // namespace

TEST_CASE ("The multiband's editor shows each band squeezed, and its drags set the bands' gains and splits")
{
    Song song (Multiband::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::MultibandEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::MultibandEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, Multiband::info);
    phazite::test::checkChildrenApart (editor);
    phazite::test::checkChildrenApart (editor.getDisplay());

    // A loud low sine: the low band comes in loud, and is turned down; the others hear nothing.
    auto* multiband = device.getProcessor<Multiband>();
    REQUIRE (multiband != nullptr);
    playLowSine (*multiband, Multiband::info, -6.0f);
    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow();
    CHECK (editor.getShownLevel (0) > -12.0f);
    CHECK (editor.getShownChange (0) < -3.0f);
    CHECK (editor.getShownLevel (2) < editor.getShownLevel (0) - 40.0f); // its start's click, at most

    // A drag up the mid band's column raises its gain, as one undo step; a double-click puts it back.
    auto& display = editor.getDisplay();
    phazite::test::Mouse mouse (display);
    const auto mid = editor.getBandArea (1).getCentre();
    mouse.press (mid);
    mouse.dragTo (mid - juce::Point<int> (0, 10));
    mouse.dragTo (mid - juce::Point<int> (0, 20));
    mouse.release();
    CHECK (song.valueOf ("midGain") > 1.0f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Mid Gain"));
    mouse.pause();
    mouse.doubleClick (mid);
    CHECK (song.valueOf ("midGain") == doctest::Approx (0.0f));

    // A drag across the spectrum moves the split nearest: the high one, to the right.
    const auto highX = editor.getCrossoverX (1);
    mouse.pause();
    mouse.press ({ highX - 2, 8 });
    mouse.dragTo ({ highX + 10, 8 });
    mouse.dragTo ({ highX + 20, 8 });
    mouse.release();
    CHECK (song.valueOf ("highFrequency") > 2500.0f);
    CHECK (song.valueOf ("lowFrequency") == doctest::Approx (90.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change High Crossover"));

    // As wide at the dock's least height, every part inside it.
    CHECK (editor.getWidthForHeight (editor.getMinimumHeight()) == editor.getWidth());
    window.setSize (editor.getWidth(), editor.getMinimumHeight());
    phazite::test::checkChildrenApart (editor);
}

TEST_CASE ("The maximizer's editor shows its bands' squeeze and its limiter, and a drag sets a band's level")
{
    Song song (Maximizer::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::MaximizerEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::MaximizerEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, Maximizer::info);
    phazite::test::checkChildrenApart (editor);
    phazite::test::checkChildrenApart (editor.getBands());

    // A loud low sine: the low band is squeezed, and the limiter turns it down to the ceiling.
    auto* maximizer = device.getProcessor<Maximizer>();
    REQUIRE (maximizer != nullptr);
    playLowSine (*maximizer, Maximizer::info, -3.0f);
    editor.refresh();
    CHECK (editor.getShownLevel (0) > -6.0f);
    CHECK (editor.getShownReduction (0) > 0.5f);
    CHECK (editor.getShownLevel (2) < editor.getShownLevel (0) - 40.0f);
    CHECK (editor.getLimiterDisplay().getOutputMeter().getValue() <= -0.9f);

    // A drag up the high band's panel raises its level, as one undo step.
    phazite::test::Mouse mouse (editor.getBands());
    const auto high = editor.getBandArea (2).getCentre();
    mouse.press (high);
    mouse.dragTo (high - juce::Point<int> (0, 10));
    mouse.dragTo (high - juce::Point<int> (0, 20));
    mouse.release();
    CHECK (song.valueOf ("highLevel") > 1.0f);
    CHECK (song.describeUndo() == juce::String ("Undo Change High Level"));
}
