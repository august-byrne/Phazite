// SPDX-License-Identifier: AGPL-3.0-or-later
// The limiter's editor (src/ui/devices/LimiterEditor.h), the meters it shows (ui::ValueMeter), and the history it shares with the
// compressor (src/ui/devices/LevelHistoryView.h): its ceiling's line to drag, and its beats and bars.
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

TEST_CASE (
    "The ceiling's line shows it can be dragged: near it an up-and-down arrow, the line lit, the hint saying so; a press away moves nothing")
{
    // The owner (2026-11-10): "it should be obvious that you are able to do this gesture".
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = phazite::insertDeviceState (s, phazite::getMasterID (s), phazite::makeOwnDeviceState (s, Limiter::typeID));
    REQUIRE (id.isValid());
    phazite::getSession (song.context)->update();
    const auto ceiling = [&s, id] { return phazite::getValueSetByHand (s, phazite::ParameterRef { id, "ceiling" }); };

    phazite::ui::LimiterEditor editor (phazite::makeDeviceHandle (s, id));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& history = editor.getDisplay().getHistory();
    phazite::test::Mouse mouse (history);
    const auto y = juce::roundToInt (history.getYForLevel (ceiling()));

    // Away from the line: nothing to drag, and what the history shows is what the hint bar says.
    mouse.moveTo ({ 60, history.getHeight() - 10 });
    CHECK_FALSE (history.isLineLit());
    CHECK (history.getMouseCursor() == juce::MouseCursor::NormalCursor);
    CHECK (history.getTooltip().isEmpty());

    // On it: an up-and-down arrow, the line lit, and the drag said.
    mouse.moveTo ({ 60, y + 3 });
    CHECK (history.isLineLit());
    CHECK (history.getMouseCursor() == juce::MouseCursor::UpDownResizeCursor);
    CHECK (history.getTooltip().contains ("Drag up or down to move the ceiling"));

    // Off it again, and a drag that starts away from it moves nothing.
    mouse.moveTo ({ 60, history.getHeight() - 10 });
    CHECK_FALSE (history.isLineLit());
    mouse.press ({ 60, history.getHeight() - 10 });
    mouse.dragTo ({ 60, history.getHeight() - 40 });
    mouse.release();
    CHECK (ceiling() == doctest::Approx (-1.0f));
    CHECK_FALSE (history.isLineLit());
}

TEST_CASE ("A dynamics history's beat and bar lines are where the song passed them, and move with what it shows")
{
    // At 120 in 4/4, a reading every 50 ms (a tenth of a beat), loud on each beat, as a kick on every beat would be heard.
    phazite::ui::LevelHistoryView history ("History", -48.0f, 24.0f);
    phazite::test::TestWindow window (history, 400, 160);
    phazite::ui::LevelHistoryView::SongTime song { .beat = 0.0, .playing = true, .bpm = 120.0 };
    const auto push = [&history, &song] (int reading)
    {
        song.beat = 0.1 * reading;
        song.barStart = 4.0 * std::floor (song.beat / 4.0 + 1.0e-9);
        const auto onBeat = reading % 10 == 0;
        history.push (1000.0 + 50.0 * reading, song, onBeat ? -6.0f : -48.0f, -48.0f, 0.0f);
    };

    for (int reading = 0; reading <= 50; ++reading)
        push (reading);

    // Two bars at 120: four seconds across. A line at each beat the song passed, at the moment of the kick on it, the fourth
    // beat a bar's.
    CHECK (history.getSpanMilliseconds() == doctest::Approx (4000.0));
    auto lines = history.getLines();
    REQUIRE (lines.size() == 5);

    for (size_t n = 0; n < lines.size(); ++n)
    {
        CAPTURE (n);
        CHECK (lines[n].milliseconds == doctest::Approx (1500.0 + 500.0 * (double) n));
        CHECK (lines[n].bar == (n == 3));
    }

    // Each at the right edge's distance from the newest reading, two bars across the width; and half a second on, every line,
    // as each kick, half a second further left.
    const auto before = history.getXForTime (lines[1].milliseconds);
    const auto perMillisecond = (float) (history.getWidth() - 2) / 4000.0f;
    CHECK (before == doctest::Approx ((float) (history.getWidth() - 1) - (3500.0f - 2000.0f) * perMillisecond).epsilon (0.001));

    for (int reading = 51; reading <= 60; ++reading)
        push (reading);

    CHECK (history.getXForTime (lines[1].milliseconds) == doctest::Approx (before - 500.0f * perMillisecond).epsilon (0.001));
    lines = history.getLines();
    REQUIRE (lines.size() == 6);
    CHECK (lines.back().milliseconds == doctest::Approx (4000.0)); // the second bar's third beat

    // A loop going round, back to the first bar: a bar's line where the song came back to its start.
    song.beat = 0.04;
    song.barStart = 0.0;
    history.push (1000.0 + 50.0 * 61, song, -48.0f, -48.0f, 0.0f);
    lines = history.getLines();
    REQUIRE (lines.size() == 7);
    CHECK (lines.back().bar);
    CHECK (lines.back().milliseconds == doctest::Approx (1000.0 + 50.0 * 61 - 20.0));

    // Stopped, the song passes no beats: the lines scroll on, but none comes.
    song.playing = false;
    history.push (1000.0 + 50.0 * 62, song, -48.0f, -48.0f, 0.0f);
    CHECK (history.getLines().size() == 7);
}
