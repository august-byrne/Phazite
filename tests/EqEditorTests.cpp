// SPDX-License-Identifier: AGPL-3.0-or-later
// The EQ's editor (src/ui/devices/EqEditor.h), and the spectrum its analyzer shows (src/devices/core/Spectrum.h).
#include "GoldenFiles.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/core/Spectrum.h"
#include "devices/eq/Equalizer.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/DisplayTicker.h"
#include "ui/components/WheelBurst.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/EqEditor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
using Eq = phazite::devices::Equalizer;

// An EQ after the song's first track's effects, which the song's session plays.
phazite::DeviceID addEq (phazite::test::TestSong& song)
{
    auto& s = *phazite::findSong (song.context);
    const auto id = phazite::test::addOwnDevice (s, song.firstTrack, Eq::typeID);
    REQUIRE (id.isValid());
    phazite::getSession (song.context)->update();
    return id;
}

float valueOf (const phazite::Song& song, phazite::DeviceID eq, const char* id)
{
    return phazite::getValueSetByHand (song, phazite::ParameterRef { eq, id });
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

template <typename Control>
Control* findControl (juce::Component& root, const juce::String& title)
{
    for (auto* control : phazite::test::findAll<Control> (root))
        if (control->getTitle() == title)
            return control;

    return nullptr;
}

int countDifferentPixels (const juce::Image& a, const juce::Image& b)
{
    auto differ = 0;

    for (int y = 0; y < std::min (a.getHeight(), b.getHeight()); ++y)
        for (int x = 0; x < std::min (a.getWidth(), b.getWidth()); ++x)
            if (a.getPixelAt (x, y) != b.getPixelAt (x, y))
                ++differ;

    return differ;
}

void setValue (phazite::test::TestSong& song, phazite::DeviceID eq, const char* id, float value)
{
    phazite::setValueSetByHand (*phazite::findSong (song.context), phazite::ParameterRef { eq, id }, value);
}
} // namespace

TEST_CASE ("A spectrum shows a sine at its frequency and level, and falls slowly when it stops")
{
    phazite::devices::Spectrum spectrum;
    spectrum.setSampleRate (48000.0);
    CHECK (spectrum.getSize() == 2048); // about 43 ms
    std::vector<float> sound (4096);

    // 1.5 kHz falls on a bin: 1500 * 2048 / 48000 = 64.
    for (size_t i = 0; i < sound.size(); ++i)
        sound[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1500.0 * (double) i / 48000.0);

    spectrum.add (sound.data(), (int) sound.size());
    spectrum.update (100.0f);
    CHECK (spectrum.getLevelAt (1500.0) == doctest::Approx (-6.02).epsilon (0.02));
    CHECK (spectrum.getLevelAt (6000.0) < -60.0f);

    // Silence: the frames that still hold some of the sine, then frames of silence alone, under which the level falls by as
    // much as an update lets it.
    std::fill (sound.begin(), sound.end(), 0.0f);
    spectrum.add (sound.data(), (int) sound.size());
    spectrum.update (3.0f);
    const auto fading = spectrum.getLevelAt (1500.0);
    spectrum.add (sound.data(), (int) sound.size());
    spectrum.update (3.0f);
    CHECK (spectrum.getLevelAt (1500.0) == doctest::Approx (fading - 3.0f).epsilon (0.01));

    // At 96 kHz, frames as long: twice the samples.
    spectrum.setSampleRate (96000.0);
    CHECK (spectrum.getSize() == 4096);
}

TEST_CASE ("A spectrum shows a sub's sine as a narrow peak at its level, every sine at its level, and the lows without flicker")
{
    // The owner, 2026-11-13: a 40 Hz sine showed as a block 50 Hz wide, in levels 23 Hz apart. The lows come from longer frames
    // of the sound at about 12 kHz, their levels about 3 Hz apart, each frame an eighth of one after the last.
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        CAPTURE (rate);
        phazite::devices::Spectrum spectrum;
        spectrum.setSampleRate (rate);
        CHECK (spectrum.getLowsSpacing() > 2.5);
        CHECK (spectrum.getLowsSpacing() < 3.2);
        const auto refresh = (int) (rate / 30.0); // the samples between two of the display's refreshes

        // A second of a sine at -6 dB, as the display's refreshes take it in; the levels at each of the last refreshes.
        const auto play = [&spectrum, rate, refresh] (double hertz, std::vector<float>* lastLevels = nullptr)
        {
            std::vector<float> sound ((size_t) rate);

            for (size_t i = 0; i < sound.size(); ++i)
                sound[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hertz * (double) i / rate);

            for (size_t at = 0; at + (size_t) refresh <= sound.size(); at += (size_t) refresh)
            {
                spectrum.add (sound.data() + at, refresh);
                spectrum.update (40.0f / 30.0f);

                if (lastLevels != nullptr && at >= sound.size() / 2)
                    lastLevels->push_back (spectrum.getLevelAt (hertz));
            }
        };

        // A sub: at its level, held steady from refresh to refresh, and narrow: 20 Hz from it, far under it.
        std::vector<float> levels;
        play (40.0, &levels);
        CHECK (spectrum.getLevelAt (40.0) == doctest::Approx (-6.02).epsilon (0.1));
        CHECK (*std::max_element (levels.begin(), levels.end()) - *std::min_element (levels.begin(), levels.end()) < 0.3f);
        CHECK (spectrum.getLevelAt (20.0) < spectrum.getLevelAt (40.0) - 25.0f);
        CHECK (spectrum.getLevelAt (60.0) < spectrum.getLevelAt (40.0) - 25.0f);

        // Every sine at its level, in the lows, where the two are blended and in the highs (a sine between two of the highs'
        // levels reads up to 1.4 dB low, as a Hann window's does).
        for (const auto hertz : { 100.0, 150.0, 212.0, 300.0, 1000.0 })
        {
            CAPTURE (hertz);
            phazite::devices::Spectrum fresh;
            fresh.setSampleRate (rate);
            std::swap (spectrum, fresh);
            play (hertz);
            CHECK (spectrum.getLevelAt (hertz) > -7.6f);
            CHECK (spectrum.getLevelAt (hertz) < -5.9f);
        }
    }

    // What would fold down onto the lows as the rate is divided stays out: a sine just under 12 kHz, at 48 kHz, which a
    // quarter of the rate would put at 50 Hz.
    phazite::devices::Spectrum spectrum;
    spectrum.setSampleRate (48000.0);
    std::vector<float> high (48000);

    for (size_t i = 0; i < high.size(); ++i)
        high[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 11950.0 * (double) i / 48000.0);

    // Its second second alone, once the frames that held its start, a step that spreads over every frequency, have gone.
    spectrum.add (high.data(), (int) high.size());
    spectrum.update (100.0f);
    spectrum.add (high.data(), (int) high.size());
    spectrum.update (200.0f);
    CHECK (spectrum.getLevelAt (11950.0) > -8.0f);
    CHECK (spectrum.getLevelOver (20.0, 140.0) < -80.0f);
}

TEST_CASE ("A spectrum shows a short sound that comes and goes between two refreshes near its level, wherever it falls")
{
    // The owner, 2026-11-10: the EQ didn't show short sounds such as claps. A frame of the newest samples was read at each
    // refresh alone: a sound between two refreshes was read at a frame's edge, where its window all but hides it, or in part,
    // 9 to 12 dB under a steady sine's level; frames a quarter of a frame apart, the loudest of them shown, read it within 5.
    for (const auto before : { 0, 400, 800, 1200 })
    {
        CAPTURE (before);
        phazite::devices::Spectrum spectrum;
        spectrum.setSampleRate (48000.0);
        constexpr int refresh = 1600; // samples between two of the display's refreshes, at 30 a second

        // Silence, then 15 ms of a 1.5 kHz sine at -6 dB (a steady one reads -6 dB), ending so long before a refresh.
        std::vector<float> sound (4 * refresh, 0.0f);
        const auto end = 3 * refresh - before;

        for (auto i = end - 720; i < end; ++i)
            sound[(size_t) i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1500.0 * (double) i / 48000.0);

        auto loudest = phazite::devices::Spectrum::floorDecibels;

        for (int at = 0; at < (int) sound.size(); at += refresh)
        {
            spectrum.add (sound.data() + at, refresh);
            spectrum.update (1.5f);
            loudest = std::max (loudest, spectrum.getLevelAt (1500.0));
        }

        CHECK (loudest > -12.0f);
        CHECK (loudest < -6.0f);
    }
}

TEST_CASE ("The EQ's editor shows the selected band's controls, and its analyzer listens while it is open")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    const auto device = phazite::makeDeviceHandle (s, eq);
    REQUIRE (device.getProcessor<Eq>() != nullptr);
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
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    s.getUndoManager().clearUndoHistory();

    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    phazite::test::Mouse mouse (editor.getGraph());

    // Band 4: a bell at 800 Hz, flat.
    const auto dot = editor.getDotPosition (3).roundToInt();
    mouse.press (dot);
    mouse.dragTo (dot + juce::Point<int> (60, -40));
    mouse.release();
    CHECK (editor.getSelectedBand() == 3);
    CHECK (valueOf (s, eq, "band4Frequency") > 900.0f);
    CHECK (valueOf (s, eq, "band4Gain") > 3.0f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Move Band 4"));

    phazite::undo (s);
    editor.refresh();
    CHECK (valueOf (s, eq, "band4Frequency") == doctest::Approx (800.0f));
    CHECK (valueOf (s, eq, "band4Gain") == doctest::Approx (0.0f));

    // The wheel narrows or widens it.
    mouse.pause();
    mouse.wheel (editor.getDotPosition (3).roundToInt(), 0.5f);
    CHECK (valueOf (s, eq, "band4Q") == doctest::Approx (2.0f));
    phazite::ui::WheelBurst::endAll();

    // A double-click puts back its frequency, gain and Q, exactly, as one undo step named as its drag is; the band stays on
    // (on and off are its menu's and its switch's, since S6.9).
    mouse.press (editor.getDotPosition (3).roundToInt());
    mouse.dragTo (editor.getDotPosition (3).roundToInt() + juce::Point<int> (40, -30));
    mouse.release();
    mouse.pause();
    editor.refresh();
    REQUIRE (valueOf (s, eq, "band4Frequency") > 900.0f);
    mouse.doubleClick (editor.getDotPosition (3).roundToInt());

    for (const auto* id : { "band4Frequency", "band4Gain", "band4Q" })
    {
        CAPTURE (id);
        CHECK (valueOf (s, eq, id) == *phazite::makeDeviceHandle (s, eq).getParameter (id).getDefaultValue());
    }

    CHECK (valueOf (s, eq, "band4On") == 1.0f);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Move Band 4"));
    phazite::undo (s);
    editor.refresh();
    CHECK (valueOf (s, eq, "band4Frequency") > 900.0f);
    CHECK (valueOf (s, eq, "band4Q") == doctest::Approx (2.0f));
}

TEST_CASE ("The EQ's graph runs from 20 Hz to 20 kHz evenly by octaves across, and from -24 to +24 dB up it, 0 dB in the middle")
{
    // The axes as the graph says it draws them, apart from its code: a drag of band 4's dot toward the pixel nearest 1 kHz and
    // +6 dB, then toward the one nearest 5 kHz and -12 dB, moves the dot as far as the pointer went (from where the dot was, not
    // from the pixel pressed: known bug 116) and lands on the values where the dot is drawn then.
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& graph = editor.getGraph();
    const auto width = (double) graph.getWidth(), height = (double) graph.getHeight();

    for (const auto& [hz, decibels] : { std::pair (1000.0, 6.0), std::pair (5000.0, -12.0) })
    {
        CAPTURE (hz);
        const juce::Point<int> target (juce::roundToInt (std::log (hz / 20.0) / std::log (1000.0) * width),
                                       juce::roundToInt ((0.5 - decibels / 48.0) * height));
        phazite::test::Mouse mouse (graph);
        const auto dot = editor.getDotPosition (3);
        const auto pressed = dot.roundToInt();
        mouse.press (pressed);
        mouse.dragTo (target);
        mouse.release();
        mouse.pause();
        const auto landed = dot + (target - pressed).toFloat();

        CHECK (valueOf (s, eq, "band4Frequency") == doctest::Approx (20.0 * std::pow (1000.0, landed.x / width)).epsilon (0.002));
        CHECK (valueOf (s, eq, "band4Frequency") == doctest::Approx (hz).epsilon (0.03)); // a pixel is under 3 % of a frequency
        CHECK (valueOf (s, eq, "band4Gain") == doctest::Approx ((0.5 - landed.y / height) * 48.0).epsilon (0.002));
        CHECK (std::abs (valueOf (s, eq, "band4Gain") - decibels) < 48.0 / height); // within a pixel's decibels
        CHECK (editor.getDotPosition (3).getDistanceFrom (landed) < 0.05f);
    }
}

TEST_CASE ("The pointer over an EQ band's dot shows it can be grabbed, and the dot lights up")
{
    // The owner, 2026-11-10: over what a drag moves in a device's graph, the pointer didn't change, so it wasn't plain that
    // it could be dragged.
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& graph = editor.getGraph();
    phazite::test::Mouse mouse (graph);

    // Away from every dot: the plain pointer.
    mouse.moveTo ({ 2, 2 });
    CHECK (graph.getMouseCursor() == juce::MouseCursor (juce::MouseCursor::NormalCursor));

    // Over band 4's dot: the hand, and a ring a pixel further out than the dot's own, which lights pixels around the dot that
    // were the graph's (counted around it: one pixel may be the curve's line, which crosses a flat band's dot).
    const auto dot = editor.getDotPosition (3).roundToInt();
    const auto reach = juce::roundToInt (phazite::ui::GraphTint::dotRadius) + 4;
    const auto before = graph.createComponentSnapshot (graph.getLocalBounds());
    mouse.moveTo (dot);
    CHECK (graph.getMouseCursor() == juce::MouseCursor (juce::MouseCursor::DraggingHandCursor));
    const auto after = graph.createComponentSnapshot (graph.getLocalBounds());
    auto lit = 0;

    for (auto y = dot.y - reach; y <= dot.y + reach; ++y)
        for (auto x = dot.x - reach; x <= dot.x + reach; ++x)
            if (after.getPixelAt (x, y) != before.getPixelAt (x, y))
                ++lit;

    CHECK (lit > 8);
}

TEST_CASE ("The EQ's editor has the selected band's Placement, Dynamic and Threshold, the Threshold faint while the Dynamic is 0")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    auto* placement = findControl<juce::ComboBox> (editor, "Placement");
    auto* dynamic = findControl<phazite::ui::Knob> (editor, "Dynamic");
    auto* threshold = findControl<phazite::ui::Knob> (editor, "Threshold");
    REQUIRE (placement != nullptr);
    REQUIRE (dynamic != nullptr);
    REQUIRE (threshold != nullptr);
    CHECK (placement->getNumItems() == 5);
    CHECK (placement->getText() == juce::String ("Stereo"));
    CHECK (dynamic->getValue() == doctest::Approx (0.0));
    CHECK (threshold->getValue() == doctest::Approx (-24.0));
    CHECK (threshold->isEnabled());
    CHECK (threshold->getAlpha() < 1.0f);
    phazite::test::checkChildrenApart (editor);

    // Picking a placement sets the band's, as one undo step.
    s.getUndoManager().clearUndoHistory();
    placement->setSelectedItemIndex (1, juce::sendNotificationSync);
    CHECK (valueOf (s, eq, "band3Placement") == doctest::Approx (1.0f));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Band 3 Placement"));

    // A Dynamic: the Threshold as strong as the rest.
    setValue (song, eq, "band3Dynamic", -6.0f);
    editor.refresh();
    CHECK (dynamic->getValue() == doctest::Approx (-6.0));
    CHECK (juce::exactlyEqual (threshold->getAlpha(), 1.0f));

    // A cut ignores both.
    editor.selectBand (0);
    CHECK_FALSE (dynamic->isEnabled());
    CHECK_FALSE (threshold->isEnabled());
    CHECK (placement->isEnabled());
}

TEST_CASE ("The EQ's editor picks its Phase with two buttons, and shows the latency Linear plays at")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    auto* minimum = findControl<juce::Button> (editor, "Phase: Minimum");
    auto* linear = findControl<juce::Button> (editor, "Phase: Linear");
    REQUIRE (minimum != nullptr);
    REQUIRE (linear != nullptr);
    CHECK (minimum->getToggleState());
    CHECK (editor.getLatencyText().isEmpty());
    phazite::test::checkChildrenApart (editor);

    // Linear, as one undo step: the device made anew at its latency, which shows beside the choice.
    s.getUndoManager().clearUndoHistory();
    linear->triggerClick();
    phazite::test::dispatchPendingMessages();
    CHECK (valueOf (s, eq, "phase") == doctest::Approx (1.0f));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Phase"));
    phazite::getSession (song.context)->update();
    editor.refresh();
    const auto handle = phazite::makeDeviceHandle (s, eq);
    auto* processor = handle.getProcessor<Eq>();
    REQUIRE (processor != nullptr);
    REQUIRE (processor->isLinear());
    const auto expected = juce::String (juce::roundToInt (1000.0 * processor->getLatencySamples() / processor->getSampleRate())) + " ms";
    CHECK (editor.getLatencyText() == expected);
    CHECK (linear->getToggleState());

    // Back to Minimum: no latency, nothing shown.
    phazite::undo (s);
    phazite::getSession (song.context)->update();
    editor.refresh();
    CHECK (editor.getLatencyText().isEmpty());
}

TEST_CASE ("An EQ band off Stereo shows its placement's letter in its dot, and the graph draws the curves of the ways through the EQ")
{
    using Placement = Eq::Placement;
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // Every band on Stereo: one curve, and the numbers alone.
    CHECK (editor.getCurvesShown() == std::vector { Placement::stereo });
    CHECK (editor.getDotText (2) == juce::String ("3"));
    const auto paint = [&editor]
    {
        auto& graph = editor.getGraph();
        return graph.createComponentSnapshot (graph.getLocalBounds());
    };

    const auto oneCurve = paint();
    const auto workedOut = editor.getCurvesWorkedOut();

    // A band on Mid: the middle's curve and the side's, and an M in its dot.
    setValue (song, eq, "band3Gain", 6.0f);
    setValue (song, eq, "band3Placement", (float) Placement::mid);
    editor.refresh();
    CHECK (editor.getCurvesShown() == std::vector { Placement::mid, Placement::side });
    CHECK (editor.getDotText (2) == juce::String ("3M"));
    const auto twoCurves = paint();
    CHECK (editor.getCurvesWorkedOut() > workedOut);
    CHECK (countDifferentPixels (oneCurve, twoCurves) > 100);

    // And one on Left: the left's and the right's too; one on Side, Right: S, R.
    setValue (song, eq, "band5Placement", (float) Placement::left);
    setValue (song, eq, "band6Placement", (float) Placement::side);
    setValue (song, eq, "band7Placement", (float) Placement::right);
    editor.refresh();
    CHECK (editor.getCurvesShown() == std::vector { Placement::mid, Placement::side, Placement::left, Placement::right });
    CHECK (editor.getDotText (4) == juce::String ("5L"));
    CHECK (editor.getDotText (5) == juce::String ("6S"));
    CHECK (editor.getDotText (6) == juce::String ("7R"));

    // A band that is off counts for none.
    for (const auto* on : { "band3On", "band6On" })
        setValue (song, eq, on, 0.0f);

    editor.refresh();
    CHECK (editor.getCurvesShown() == std::vector { Placement::left, Placement::right });
}

TEST_CASE ("The EQ's analyzer shows a trace for each way whose curve it draws, in its curve's colour, each of that way's sound alone")
{
    using Placement = Eq::Placement;
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();
    auto* processor = phazite::makeDeviceHandle (s, eq).getProcessor<Eq>();
    REQUIRE (processor != nullptr);
    ticker.tickNow(); // it listens

    // What the EQ plays, as the audio thread taps it, at its rate: a 1 kHz sine on each side, alike (the middle alone) or
    // opposite (the side).
    const auto rate = processor->getSampleRate();
    const auto play = [&] (float left, float right)
    {
        std::vector<float> lefts (8192), rights (8192);

        for (size_t i = 0; i < lefts.size(); ++i)
        {
            const auto sine = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) i / rate);
            lefts[i] = left * sine;
            rights[i] = right * sine;
        }

        processor->getOutputTap().push (lefts.data(), rights.data(), (int) lefts.size());
        ticker.tickNow();
    };
    const auto shows = [&editor] (Placement way) { return editor.getSpectrumLevel (way, 1000.0) > -20.0f; };
    const auto showsNothing = [&editor] (Placement way) { return editor.getSpectrumLevel (way, 1000.0) < -80.0f; };

    // A band on Side: the middle's and the side's curves, and a trace for each, in its curve's colour.
    setValue (song, eq, "band3Placement", (float) Placement::side);
    ticker.tickNow();
    REQUIRE (editor.getCurvesShown() == std::vector { Placement::mid, Placement::side });

    for (const auto way : { Placement::mid, Placement::side })
        CHECK (editor.getSpectrumColour (way) == editor.getCurveColour (way));

    CHECK (editor.getCurveColour (Placement::mid) != editor.getCurveColour (Placement::side));

    // A sound in the middle alone shows in the middle's trace and not the side's; one on the side alone, the reverse.
    play (1.0f, 1.0f);
    CHECK (shows (Placement::mid));
    CHECK (showsNothing (Placement::side));

    editor.setVisible (false); // afresh, as when it shows again
    ticker.tickNow();
    editor.setVisible (true);
    ticker.tickNow();
    play (1.0f, -1.0f);
    CHECK (shows (Placement::side));
    CHECK (showsNothing (Placement::mid));

    // A band on Left: the left's and the right's traces as well; a sound on the left alone in the left's.
    setValue (song, eq, "band3Placement", (float) Placement::left);
    ticker.tickNow();
    REQUIRE (editor.getCurvesShown() == std::vector { Placement::left, Placement::right });
    CHECK (editor.getSpectrumColour (Placement::left) == editor.getCurveColour (Placement::left));
    CHECK (editor.getSpectrumColour (Placement::right) == editor.getCurveColour (Placement::right));
    editor.setVisible (false);
    ticker.tickNow();
    editor.setVisible (true);
    ticker.tickNow();
    play (1.0f, 0.0f);
    CHECK (shows (Placement::left));
    CHECK (showsNothing (Placement::right));
    play (0.0f, 1.0f);
    CHECK (shows (Placement::right));

    // Every band on Stereo: one trace, the louder of the two sides, in the device's colour.
    setValue (song, eq, "band3Placement", (float) Placement::stereo);
    ticker.tickNow();
    REQUIRE (editor.getCurvesShown() == std::vector { Placement::stereo });
    CHECK (shows (Placement::stereo));
}

TEST_CASE ("The EQ's analyzer, hidden while a loud sound plays and shown again in silence, shows the silence now")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();
    auto* processor = phazite::makeDeviceHandle (s, eq).getProcessor<Eq>();
    REQUIRE (processor != nullptr);
    ticker.tickNow();

    std::vector<float> sine (8192);

    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) i / processor->getSampleRate());

    processor->getOutputTap().push (sine.data(), sine.data(), (int) sine.size());
    ticker.tickNow();
    CHECK (editor.getSpectrumLevel (Eq::Placement::stereo, 1000.0) > -20.0f);

    editor.setVisible (false);
    ticker.tickNow();
    processor->getOutputTap().push (sine.data(), sine.data(), (int) sine.size());
    editor.setVisible (true);
    ticker.tickNow();
    CHECK (editor.getSpectrumLevel (Eq::Placement::stereo, 1000.0) < -100.0f);
}

TEST_CASE ("The EQ's curves are what is heard at its Mix: a cut half filled in at 50 %, the middle's and the side's alike")
{
    using Placement = Eq::Placement;
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // A bell 12 dB down at 1 kHz on the side: the side's curve dips 12 dB at 100 %, and at 50 % to what is heard, -4.1 dB in
    // Minimum (the dry sound half, the bell's phase none at its middle), the middle's flat.
    setValue (song, eq, "band3Type", (float) Eq::BandType::bell);
    setValue (song, eq, "band3Frequency", 1000.0f);
    setValue (song, eq, "band3Gain", -12.0f);
    setValue (song, eq, "band3Placement", (float) Placement::side);
    editor.refresh();
    CHECK (editor.getCurveDecibels (Placement::side, 1000.0) == doctest::Approx (-12.0f).epsilon (0.01));

    setValue (song, eq, "mix", 0.5f);
    editor.refresh();
    const auto expected = (float) (20.0 * std::log10 (0.5 * (1.0 + std::pow (10.0, -12.0 / 20.0))));
    CHECK (editor.getCurveDecibels (Placement::side, 1000.0) == doctest::Approx (expected).epsilon (0.01));
    CHECK (editor.getCurveDecibels (Placement::mid, 1000.0) == doctest::Approx (0.0f).scale (1.0f).epsilon (0.01));

    // Each point of each way as Eq::getHeardMagnitude() says, which a sine through it measures (EqualizerTests.cpp), in Linear too.
    for (const auto phase : { 0.0f, 1.0f })
    {
        setValue (song, eq, "phase", phase);
        editor.refresh();
        std::vector<float> values;

        for (const auto& parameter : Eq::info.parameters)
            values.push_back (valueOf (s, eq, parameter.id.data()));

        for (const auto way : { Placement::mid, Placement::side })
            for (const auto hertz : { 300.0, 900.0, 1100.0, 5000.0 })
                CHECK (editor.getCurveDecibels (way, hertz)
                       == doctest::Approx (20.0 * std::log10 (Eq::getHeardMagnitude (values, hertz, 48000.0, way))).epsilon (0.01));
    }
}

TEST_CASE ("A dynamic EQ band's handle drags its Dynamic as one undo step, and its mark and the curve follow its gain as it plays")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    const auto device = phazite::makeDeviceHandle (s, eq);
    phazite::ui::EqEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // No handle until the band is dynamic; then one where its gain goes at its whole Dynamic, under its dot.
    CHECK_FALSE (editor.getDynamicHandlePosition (3).has_value());
    setValue (song, eq, "band4Gain", 3.0f);
    setValue (song, eq, "band4Dynamic", -9.0f);
    editor.refresh();
    const auto handle = editor.getDynamicHandlePosition (3);
    REQUIRE (handle.has_value());
    const auto height = (double) editor.getGraph().getHeight();
    CHECK (handle->x == doctest::Approx (editor.getDotPosition (3).x));
    CHECK (handle->y == doctest::Approx ((0.5 - (3.0 - 9.0) / 48.0) * height));

    // Dragged up 30 pixels: the Dynamic up by as many decibels, the band's frequency and gain as they were, one undo step.
    s.getUndoManager().clearUndoHistory();
    phazite::test::Mouse mouse (editor.getGraph());
    mouse.moveTo (handle->roundToInt());
    CHECK (editor.getGraph().getMouseCursor() == juce::MouseCursor (juce::MouseCursor::UpDownResizeCursor));
    mouse.press (handle->roundToInt());
    mouse.dragTo (handle->roundToInt() + juce::Point<int> (0, -30));
    mouse.release();
    mouse.pause();
    CHECK (valueOf (s, eq, "band4Dynamic") == doctest::Approx (-9.0 + 30.0 * 48.0 / height).epsilon (0.01));
    CHECK (valueOf (s, eq, "band4Gain") == doctest::Approx (3.0f));
    CHECK (valueOf (s, eq, "band4Frequency") == doctest::Approx (800.0f));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Band 4 Dynamic"));
    phazite::undo (s);
    CHECK (valueOf (s, eq, "band4Dynamic") == doctest::Approx (-9.0f));
    editor.refresh();

    // A loud sound in the band: the gain it plays goes down, and the curve with it.
    auto* processor = device.getProcessor<Eq>();
    REQUIRE (processor != nullptr);
    processor->prepare (48000.0, 480);
    std::vector<float> values;

    for (const auto& parameter : Eq::info.parameters)
        values.push_back (valueOf (s, eq, parameter.id.data())); // the IDs are literals

    const auto before = editor.getCurvesWorkedOut();
    CHECK (editor.getShownGain (3) == doctest::Approx (3.0f));
    juce::AudioBuffer<float> buffer (2, 480);

    for (int block = 0; block < 50; ++block)
    {
        for (int i = 0; i < 480; ++i)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i,
                                  0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 800.0 * (block * 480 + i) / 48000.0));

        phazite::devices::ProcessContext context;
        context.channels = buffer.getArrayOfWritePointers();
        context.numChannels = 2;
        context.numSamples = buffer.getNumSamples();
        context.parameters = values;
        processor->process (context);
    }

    editor.refresh();
    editor.getGraph().createComponentSnapshot (editor.getGraph().getLocalBounds()); // paints, working the curves out
    CHECK (editor.getShownGain (3) == doctest::Approx (-6.0f).epsilon (0.02));
    CHECK (editor.getCurvesWorkedOut() > before);
}

TEST_CASE ("A small Dynamic's handle, within a dot's reach, is taken where it is pressed; and Make Dynamic gives a band one")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // Make Dynamic, as the dot's right-click menu offers it: -6 dB, one undo step.
    s.getUndoManager().clearUndoHistory();
    editor.makeDynamic (3);
    CHECK (valueOf (s, eq, "band4Dynamic") == doctest::Approx (-6.0f));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Make Band 4 Dynamic"));

    // A Dynamic of -2 dB: its handle about 12 pixels under the dot, within the dot's reach. Pressed on the handle, the drag
    // moves the Dynamic; pressed on the dot, the band.
    setValue (song, eq, "band4Dynamic", -2.0f);
    editor.refresh();
    const auto dot = editor.getDotPosition (3);
    const auto handle = editor.getDynamicHandlePosition (3);
    REQUIRE (handle.has_value());
    CHECK (handle->getDistanceFrom (dot) < phazite::ui::GraphTint::dotRadius * 1.5f);

    phazite::test::Mouse mouse (editor.getGraph());
    mouse.press (handle->roundToInt());
    mouse.dragTo (handle->roundToInt() + juce::Point<int> (0, 20));
    mouse.release();
    mouse.pause();
    CHECK (valueOf (s, eq, "band4Dynamic") < -3.0f);
    CHECK (valueOf (s, eq, "band4Frequency") == doctest::Approx (800.0f));
    CHECK (valueOf (s, eq, "band4Gain") == doctest::Approx (0.0f));

    mouse.press (dot.roundToInt());
    mouse.dragTo (dot.roundToInt() + juce::Point<int> (0, -20));
    mouse.release();
    CHECK (valueOf (s, eq, "band4Gain") > 2.0f);
}
