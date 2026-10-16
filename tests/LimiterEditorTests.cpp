// SPDX-License-Identifier: AGPL-3.0-or-later
// The limiter's editor (src/ui/devices/LimiterEditor.h) and the meters it shows (ui::ValueMeter).
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Limiter.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/LimiterEditor.h"

#include <vector>

namespace
{
using phazite::devices::Limiter;

// Plays a full-scale sine through the limiter's DSP, pushed 12 dB, as the audio thread would.
void playLoud (Limiter& limiter, int numSamples)
{
    std::vector<float> values;

    for (const auto& parameter : Limiter::info.parameters)
        values.push_back (parameter.defaultValue);

    values[Limiter::gain] = 12.0f;
    juce::AudioBuffer<float> buffer (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
        for (int channel = 0; channel < 2; ++channel)
            buffer.setSample (channel, i, (float) std::sin (juce::MathConstants<double>::twoPi * 997.0 * i / 48000.0));

    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = numSamples;
    block.parameters = values;
    limiter.process (block);
}
} // namespace

TEST_CASE ("A value meter jumps up and falls back smoothly, and screen readers read it")
{
    phazite::ui::ValueMeter level ("Output", -48.0f, 0.0f, phazite::ui::ValueMeter::Fill::fromBottom);
    CHECK (level.getValue() == doctest::Approx (-48.0f));

    level.update (-6.0f);
    CHECK (level.getValue() == doctest::Approx (-6.0f));
    level.update (-120.0f); // nothing came
    CHECK (level.getValue() == doctest::Approx (-6.5f));
    level.update (12.0f); // over the top stays at the top
    CHECK (level.getValue() == doctest::Approx (0.0f));

    phazite::ui::ValueMeter reduction ("Gain Reduction", 0.0f, 24.0f, phazite::ui::ValueMeter::Fill::fromTop);
    reduction.update (4.0f);
    CHECK (reduction.getValueText() == juce::String ("-4.0 dB"));
    CHECK (level.getValueText() == juce::String ("0.0 dB"));
}

TEST_CASE ("The limiter's editor shows its knobs and meters, with the ceiling marked")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (Limiter::typeID, {});
    REQUIRE (phazite::insertMasterDevice (*edit, plugin) != nullptr);
    phazite::test::settle();

    const auto device = phazite::makeDeviceHandle (*plugin);
    CHECK (dynamic_cast<phazite::ui::LimiterEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    auto* limiter = device.getProcessor<Limiter>();
    REQUIRE (limiter != nullptr);
    limiter->prepare (48000.0, 48000);

    phazite::ui::LimiterEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    CHECK (phazite::test::findAll<phazite::ui::Knob> (editor).size() == Limiter::info.parameters.size());
    phazite::test::checkChildrenApart (editor);

    // A full-scale sine pushed 12 dB comes in at +12 dB, over the meter's top, goes down 13 dB, and out at the ceiling.
    playLoud (*limiter, 48000);
    editor.refresh();
    CHECK (editor.getInputMeter().getValue() == doctest::Approx (0.0f));
    CHECK (editor.getReductionMeter().getValue() == doctest::Approx (13.0f).epsilon (0.03));
    CHECK (editor.getOutputMeter().getValue() == doctest::Approx (-1.0f).epsilon (0.01));

    // Turning a knob is an undo step the model names.
    auto* ceilingKnob = phazite::test::findAll<phazite::ui::Knob> (editor)[Limiter::ceiling];
    ceilingKnob->setValue (-3.0, juce::sendNotificationSync);
    CHECK (plugin->getAutomatableParameterByID ("ceiling")->getCurrentValue() == doctest::Approx (-3.0f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Ceiling"));
}
