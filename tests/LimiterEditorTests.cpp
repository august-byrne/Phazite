// SPDX-License-Identifier: AGPL-3.0-or-later
// The limiter's editor (src/ui/devices/LimiterEditor.h) and the meters it shows (ui::ValueMeter).
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Limiter.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
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

TEST_CASE ("The limiter's editor shows its knobs, meters and history, with the ceiling a line to drag")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = phazite::insertDeviceState (s, phazite::getMasterID (s), phazite::makeOwnDeviceState (s, Limiter::typeID));
    REQUIRE (id.isValid());
    phazite::getSession (song.context)->update();
    const auto ceiling = [&s, id] { return phazite::getValueSetByHand (s, phazite::ParameterRef { id, "ceiling" }); };

    const auto device = phazite::makeDeviceHandle (s, id);
    CHECK (dynamic_cast<phazite::ui::LimiterEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    auto* limiter = device.getProcessor<Limiter>();
    REQUIRE (limiter != nullptr);
    limiter->prepare (48000.0, 48000);

    phazite::ui::LimiterEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // A knob for each parameter: it has no mix, which would let peaks past its ceiling.
    CHECK (phazite::test::findAll<phazite::ui::Knob> (editor).size() == Limiter::info.parameters.size());
    CHECK (editor.findControl ("mix") == nullptr);
    phazite::test::checkChildrenApart (editor);
    phazite::test::checkChildrenApart (editor.getDisplay());

    // A full-scale sine pushed 12 dB comes in at +12 dB, over the meter's top, goes down 13 dB, and out at the ceiling; the
    // history shows how far it went down.
    playLoud (*limiter, 48000);
    editor.refresh();
    const auto& display = editor.getDisplay();
    CHECK (display.getInputMeter().getValue() == doctest::Approx (0.0f));
    CHECK (display.getReductionMeter().getValue() == doctest::Approx (13.0f).epsilon (0.03));
    CHECK (display.getOutputMeter().getValue() == doctest::Approx (-1.0f).epsilon (0.01));
    CHECK (editor.getDisplay().getHistory().getNewestReduction() == doctest::Approx (13.0f).epsilon (0.03));

    // Turning a knob is an undo step the model names.
    auto* ceilingKnob = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("ceiling"));
    REQUIRE (ceilingKnob != nullptr);
    ceilingKnob->setValue (-3.0, juce::sendNotificationSync);
    CHECK (ceiling() == doctest::Approx (-3.0f));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Ceiling"));

    // Dragging the ceiling's line down in the history lowers it, as one undo step; a double-click puts it back.
    auto& history = editor.getDisplay().getHistory();
    const auto y = juce::roundToInt (history.getYForLevel (-3.0f));
    phazite::test::Mouse mouse (history);
    mouse.press ({ 40, y });
    mouse.dragTo ({ 40, y + 10 });
    mouse.dragTo ({ 40, y + 20 });
    mouse.release();
    const auto lowered = ceiling();
    CHECK (lowered < -4.0f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Ceiling"));
    phazite::undo (s);
    CHECK (ceiling() == doctest::Approx (-3.0f));

    mouse.pause();
    mouse.doubleClick ({ 40, y });
    CHECK (ceiling() == doctest::Approx (-1.0f));
}
