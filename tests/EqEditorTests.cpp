// SPDX-License-Identifier: AGPL-3.0-or-later
// The EQ's editor (src/ui/devices/EqEditor.h), and the spectrum its analyzer shows (src/devices/core/Spectrum.h).
#include "GoldenFiles.h"
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
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/EqEditor.h"

#include <cmath>
#include <vector>

namespace
{
using Eq = phazite::devices::Equalizer;

// An EQ after the song's first track's effects, which the song's session plays.
phazite::DeviceID addEq (phazite::test::TestSong& song)
{
    auto& s = *phazite::findSong (song.context);
    const auto id = phazite::insertDeviceState (s, song.firstTrack, phazite::makeOwnDeviceState (s, Eq::typeID));
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

    // A double-click turns the band off, and on again.
    mouse.pause();
    mouse.doubleClick (editor.getDotPosition (3).roundToInt());
    CHECK (valueOf (s, eq, "band4On") == doctest::Approx (0.0f));
    mouse.doubleClick (editor.getDotPosition (3).roundToInt());
    CHECK (valueOf (s, eq, "band4On") == doctest::Approx (1.0f));

    // The wheel narrows or widens it.
    mouse.wheel (editor.getDotPosition (3).roundToInt(), 0.5f);
    CHECK (valueOf (s, eq, "band4Q") == doctest::Approx (2.0f));
}

TEST_CASE ("The EQ's graph runs from 20 Hz to 20 kHz evenly by octaves across, and from -24 to +24 dB up it, 0 dB in the middle")
{
    // The axes as the graph says it draws them, apart from its code: a drag of band 4's dot to the pixel nearest 1 kHz and
    // +6 dB, then to the one nearest 5 kHz and -12 dB, lands on the values at those pixels, the dot under the pointer.
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto eq = addEq (song);
    phazite::ui::EqEditor editor ((phazite::makeDeviceHandle (s, eq)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& graph = editor.getGraph();
    const auto width = (double) graph.getWidth(), height = (double) graph.getHeight();

    for (const auto [hz, decibels] : { std::pair (1000.0, 6.0), std::pair (5000.0, -12.0) })
    {
        CAPTURE (hz);
        const juce::Point<int> target (juce::roundToInt (std::log (hz / 20.0) / std::log (1000.0) * width),
                                       juce::roundToInt ((0.5 - decibels / 48.0) * height));
        phazite::test::Mouse mouse (graph);
        mouse.press (editor.getDotPosition (3).roundToInt());
        mouse.dragTo (target);
        mouse.release();
        mouse.pause();

        CHECK (valueOf (s, eq, "band4Frequency") == doctest::Approx (20.0 * std::pow (1000.0, target.x / width)).epsilon (0.002));
        CHECK (valueOf (s, eq, "band4Frequency") == doctest::Approx (hz).epsilon (0.03)); // a pixel is under 3 % of a frequency
        CHECK (valueOf (s, eq, "band4Gain") == doctest::Approx ((0.5 - target.y / height) * 48.0).epsilon (0.002));
        CHECK (std::abs (valueOf (s, eq, "band4Gain") - decibels) < 48.0 / height); // within a pixel's decibels
        CHECK (editor.getDotPosition (3).getDistanceFrom (target.toFloat()) < 0.5f);
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
