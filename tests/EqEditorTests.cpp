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

    // Over band 4's dot: the hand, and a ring a pixel further out than the dot's own.
    const auto dot = editor.getDotPosition (3).roundToInt();
    const auto ring = dot + juce::Point<int> (juce::roundToInt (phazite::ui::GraphTint::dotRadius) + 1, 0);
    const auto before = graph.createComponentSnapshot (graph.getLocalBounds());
    mouse.moveTo (dot);
    CHECK (graph.getMouseCursor() == juce::MouseCursor (juce::MouseCursor::DraggingHandCursor));
    CHECK (graph.createComponentSnapshot (graph.getLocalBounds()).getPixelAt (ring.x, ring.y) != before.getPixelAt (ring.x, ring.y));
}
