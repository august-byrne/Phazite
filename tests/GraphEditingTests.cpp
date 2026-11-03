// SPDX-License-Identifier: AGPL-3.0-or-later
// The pictures in Phazite's device and modulator editors as controls (the owner, 2026-11-01: move "the key points around"
// and "scroll on lines in order to bend them"): an envelope's points, an LFO's depth and rate, a compressor's curve, the
// stereo expander's fan and a breakpoint envelope's points and lines. Each drag is one undo step.
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dynamics/Compressor.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
#include "model/automation/AutomationModel.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/ModulatorEditor.h"
#include "ui/devices/StereoExpanderEditor.h"
#include "ui/devices/SynthParts.h"
#include "ui/theme/DesignTokens.h"

#include <utility>

namespace
{
constexpr float notchUp = 0.1f; // the wheel turned a notch up, as JUCE reports it

float valueOf (phazite::test::TestSong& song, phazite::DeviceID device, const char* parameterID)
{
    return phazite::getParameter (song.context, { device, parameterID }).getValue();
}

juce::String describeUndo (phazite::test::TestSong& song)
{
    return phazite::describeUndo (phazite::findSong (song.context));
}

void undo (phazite::test::TestSong& song)
{
    phazite::undo (*phazite::getEdit (song.context));
}
} // namespace

TEST_CASE ("An envelope's points drag in its picture, each drag one undo step, and a double-click puts a point back")
{
    using Point = phazite::ui::EnvelopeView::Point;
    phazite::test::TestSong song;
    const auto synth = song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    auto editor = phazite::ui::createDeviceEditor (phazite::getDeviceHandle (song.context, synth));
    REQUIRE (editor != nullptr);
    phazite::test::TestWindow window (*editor, editor->getWidth(), editor->getHeight());
    auto* envelope = dynamic_cast<phazite::ui::EnvelopeView*> (phazite::test::findComponentWithTitle (*editor, "Amp envelope"));
    REQUIRE (envelope != nullptr);
    REQUIRE (envelope->isShowing());
    const auto decay = valueOf (song, synth, "decay"), sustain = valueOf (song, synth, "sustain");

    // The corner where the decay ends, right and down: a longer decay and a lower sustain, as one step.
    phazite::test::Mouse mouse (*envelope);
    const auto corner = envelope->getPointPosition (Point::decay).roundToInt();
    mouse.press (corner);
    mouse.dragTo (corner + juce::Point<int> (20, 10));
    mouse.release();
    CHECK (valueOf (song, synth, "decay") > decay);
    CHECK (valueOf (song, synth, "sustain") < sustain);
    CHECK (describeUndo (song).startsWith ("Undo Change"));

    undo (song);
    CHECK (valueOf (song, synth, "decay") == doctest::Approx (decay));
    CHECK (valueOf (song, synth, "sustain") == doctest::Approx (sustain));

    // Dragged again, then a double-click puts it back where it started, as a knob's does.
    mouse.press (corner);
    mouse.dragTo (corner + juce::Point<int> (30, -10));
    mouse.release();
    REQUIRE (valueOf (song, synth, "decay") > decay);
    mouse.doubleClick (envelope->getPointPosition (Point::decay).roundToInt());
    CHECK (valueOf (song, synth, "decay") == doctest::Approx (decay));
    CHECK (valueOf (song, synth, "sustain") == doctest::Approx (sustain));
}

TEST_CASE ("An envelope's stages bend by the handle in their middle, the wheel leaves them be, and a double-click puts a curve back")
{
    using Point = phazite::ui::EnvelopeView::Point;
    phazite::test::TestSong song;
    const auto synth = song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    auto editor = phazite::ui::createDeviceEditor (phazite::getDeviceHandle (song.context, synth));
    REQUIRE (editor != nullptr);
    phazite::test::TestWindow window (*editor, editor->getWidth(), editor->getHeight());
    auto* envelope = dynamic_cast<phazite::ui::EnvelopeView*> (phazite::test::findComponentWithTitle (*editor, "Amp envelope"));
    REQUIRE (envelope != nullptr);
    REQUIRE (envelope->isShowing());

    // Each stage starts as an analog envelope's, which the synth played before its stages could bend.
    const auto attackCurve = valueOf (song, synth, "attackCurve"), decayCurve = valueOf (song, synth, "decayCurve");
    CHECK (attackCurve == doctest::Approx (phazite::devices::dsp::Adsr::analogAttackCurve));
    CHECK (decayCurve == doctest::Approx (phazite::devices::dsp::Adsr::analogSettleCurve));

    // The attack's handle dragged up bends it up, rising faster at first: its curve lower, as one undo step.
    REQUIRE (envelope->getBendPosition (Point::attack).has_value());
    phazite::test::Mouse mouse (*envelope);
    const auto handle = envelope->getBendPosition (Point::attack)->roundToInt();
    mouse.press (handle);
    mouse.dragTo (handle - juce::Point<int> (0, 10));
    mouse.release();
    CHECK (valueOf (song, synth, "attackCurve") < attackCurve - 0.05f);
    CHECK (describeUndo (song).startsWith ("Undo Change"));
    undo (song);
    CHECK (valueOf (song, synth, "attackCurve") == doctest::Approx (attackCurve));

    // The decay's handle dragged up bends it up too, slower at first: its curve higher.
    REQUIRE (envelope->getBendPosition (Point::decay).has_value());
    const auto decayHandle = envelope->getBendPosition (Point::decay)->roundToInt();
    mouse.press (decayHandle);
    mouse.dragTo (decayHandle - juce::Point<int> (0, 10));
    mouse.release();
    CHECK (valueOf (song, synth, "decayCurve") > decayCurve + 0.05f);

    // The wheel over it bends nothing: it is the view's, to scroll it (no envelope editor found bends with it).
    const auto bent = valueOf (song, synth, "decayCurve");
    const auto overDecay = juce::Point<int> (
        (envelope->getPointPosition (Point::attack).roundToInt().x + envelope->getPointPosition (Point::decay).roundToInt().x) / 2,
        envelope->getHeight() / 2);
    mouse.wheel (overDecay, notchUp);
    CHECK (valueOf (song, synth, "decayCurve") == doctest::Approx (bent));

    // A double-click on the decay puts its curve back.
    mouse.doubleClick (overDecay);
    CHECK (valueOf (song, synth, "decayCurve") == doctest::Approx (decayCurve));

    // A level stage has nothing to bend: a decay to a full sustain has no handle.
    phazite::getParameter (song.context, { synth, "sustain" }).setValue (1.0f);
    CHECK_FALSE (envelope->getBendPosition (Point::decay).has_value());
}

TEST_CASE ("An LFO's picture sets its depth with a drag up or down, and its rate with the wheel")
{
    phazite::test::TestSong song;
    const auto synth = song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    auto editor = phazite::ui::createDeviceEditor (phazite::getDeviceHandle (song.context, synth));
    REQUIRE (editor != nullptr);
    phazite::test::TestWindow window (*editor, editor->getWidth(), editor->getHeight());
    const auto lfos = phazite::test::findAll<phazite::ui::LfoView> (*editor);
    REQUIRE (lfos.size() == 1);
    auto& lfo = *lfos.getFirst();
    REQUIRE (lfo.isShowing());
    phazite::test::Mouse mouse (lfo);
    const auto centre = lfo.getLocalBounds().getCentre();

    // Half the picture's height up: half the depth.
    CHECK (valueOf (song, synth, "lfoDepth") == doctest::Approx (0.0f));
    mouse.press (centre);
    mouse.dragTo (centre - juce::Point<int> (0, lfo.getHeight() / 2));
    mouse.release();
    CHECK (valueOf (song, synth, "lfoDepth") == doctest::Approx (0.5f).epsilon (0.05));
    CHECK (describeUndo (song).startsWith ("Undo Change"));

    // A notch of the wheel up: a tenth faster.
    const auto rate = valueOf (song, synth, "lfoRate");
    mouse.wheel (centre, notchUp);
    CHECK (valueOf (song, synth, "lfoRate") == doctest::Approx (rate * 1.1f).epsilon (0.01));
}

TEST_CASE ("A compressor's curve is the multiband's: a point drags, a line bends by its handle, and the wheel changes nothing")
{
    using phazite::devices::Compressor;
    phazite::test::TestSong song;
    const auto device = song.addEffect (song.firstTrack, Compressor::typeID);
    phazite::ui::CompressorEditor editor (phazite::getDeviceHandle (song.context, device));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();
    phazite::test::Mouse mouse (curve);

    // Its corner at -18 dB, dragged down: it turns down more, from there on.
    const auto corner = curve.getPointPosition (1).roundToInt();
    const auto before = valueOf (song, device, "point2Y");
    mouse.press (corner, juce::ModifierKeys::ctrlModifier); // Ctrl keeps it where it is across
    mouse.dragTo (corner + juce::Point<int> (0, 20));
    mouse.release();
    CHECK (valueOf (song, device, "point2Y") < before);
    CHECK (describeUndo (song) == juce::String ("Undo Move Point"));
    undo (song);
    CHECK (valueOf (song, device, "point2Y") == doctest::Approx (before));
    editor.refresh();

    // The handle in the middle of the line over it, dragged up: the line bends up.
    REQUIRE (curve.getTensionPosition (2).has_value());
    const auto handle = curve.getTensionPosition (2)->roundToInt();
    mouse.press (handle);
    mouse.dragTo (handle - juce::Point<int> (0, 20));
    mouse.release();
    CHECK (valueOf (song, device, "point3Tension") > 0.1f);
    CHECK (describeUndo (song) == juce::String ("Undo Bend Line"));
    editor.refresh();

    // The wheel is the view's, to scroll it: no compressor's graph found turns anything with it (docs/design.md, B123).
    const auto tension = valueOf (song, device, "point3Tension"), height = valueOf (song, device, "point2Y");
    mouse.wheel (curve.getTensionPosition (2)->roundToInt(), notchUp);
    mouse.wheel (curve.getPointPosition (1).roundToInt(), notchUp);
    CHECK (valueOf (song, device, "point3Tension") == doctest::Approx (tension));
    CHECK (valueOf (song, device, "point2Y") == doctest::Approx (height));
}

TEST_CASE ("The stereo expander's fan follows a drag across its disc, and the wheel opens or closes it a step at a time")
{
    phazite::test::TestSong song;
    const auto device = song.addEffect (song.firstTrack, phazite::devices::StereoExpander::typeID);
    phazite::ui::StereoExpanderEditor editor (phazite::getDeviceHandle (song.context, device));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    phazite::test::Mouse mouse (editor.getImage());
    const auto centre = editor.getDiscCentre();
    const auto from = [centre] (float x, float y) { return (centre + juce::Point<float> (x, y)).roundToInt(); };

    // Straight up from the centre: mono; an eighth of a turn: as wide as it comes in; flat: twice as wide. One step.
    mouse.press (from (0.0f, -40.0f));
    CHECK (valueOf (song, device, "width") < 0.01f);
    mouse.dragTo (from (40.0f, -40.0f));
    CHECK (valueOf (song, device, "width") == doctest::Approx (1.0f).epsilon (0.02));
    mouse.dragTo (from (-40.0f, 0.0f));
    CHECK (valueOf (song, device, "width") == doctest::Approx (2.0f));
    mouse.release();
    CHECK (describeUndo (song) == juce::String ("Undo Change Width"));

    // A notch of the wheel down: five points narrower.
    mouse.wheel (from (0.0f, -40.0f), -notchUp);
    CHECK (valueOf (song, device, "width") == doctest::Approx (1.95f));
}

TEST_CASE ("A breakpoint envelope's points drag in its picture, its lines bend by their handles, and a double-click straightens one")
{
    phazite::test::TestSong song;
    const auto modulator = song.addModulator (song.firstTrack, phazite::ModulatorType::breakpoints);

    // Two points: from nothing up to the top at half the cycle, and down to nothing at its end.
    for (const auto& [id, value] :
         { std::pair { "numActivePoints", 2.0f }, std::pair { "stageZeroValue", 0.0f }, std::pair { "stageOneValue", 1.0f },
           std::pair { "stageOneTime", 0.5f }, std::pair { "stageOneCurve", 0.0f }, std::pair { "stageTwoValue", 0.0f },
           std::pair { "stageTwoTime", 1.0f }, std::pair { "stageTwoCurve", 0.0f } })
        song.setModulatorParameter (modulator, id, value);

    const auto valueOfPart = [&song, modulator] (const char* id)
    { return phazite::getModulatorParameter (song.context, modulator, id).getValue(); };

    phazite::ui::ModulatorEditor editor { song.context, modulator };
    phazite::test::TestWindow window { editor, editor.getWidth(), editor.getHeight() };
    auto* graph = phazite::test::findComponentWithTitle (editor, "Breakpoints");
    REQUIRE (graph != nullptr);
    phazite::test::Mouse mouse (*graph);

    // Where a time in the cycle and a level are in the picture: inside the room its handles take at its edges.
    const auto area = graph->getLocalBounds().toFloat().reduced (phazite::ui::GraphTint::handleRadius * 1.5f + 1.0f);
    const auto at = [area] (float time, float level)
    { return juce::Point<float> (area.getX() + time * area.getWidth(), area.getBottom() - level * area.getHeight()).roundToInt(); };

    // The first point, along and down, as one undo step.
    mouse.press (at (0.5f, 1.0f));
    mouse.dragTo (at (0.5f, 1.0f) + juce::Point<int> (20, 10));
    mouse.release();
    CHECK (valueOfPart ("stageOneTime") == doctest::Approx (0.5f + 20.0f / area.getWidth()).epsilon (0.01));
    CHECK (valueOfPart ("stageOneValue") == doctest::Approx (1.0f - 10.0f / area.getHeight()).epsilon (0.01));
    undo (song);
    CHECK (valueOfPart ("stageOneTime") == doctest::Approx (0.5f));
    CHECK (valueOfPart ("stageOneValue") == doctest::Approx (1.0f));

    // The handle in the middle of the first line, dragged up: the rising line bows up (a curve below 0, quick at first).
    mouse.press (at (0.25f, 0.5f));
    mouse.dragTo (at (0.25f, 0.5f) + juce::Point<int> (0, -10));
    mouse.release();
    CHECK (valueOfPart ("stageOneCurve") == doctest::Approx (-10.0f / area.getHeight()).epsilon (0.05));

    // A double-click on the line straightens it.
    mouse.doubleClick (at (0.25f, 0.5f));
    CHECK (valueOfPart ("stageOneCurve") == doctest::Approx (0.0f));

    // The wheel over the second line bends nothing: it is the view's, to scroll it.
    mouse.wheel (at (0.75f, 0.2f), notchUp);
    CHECK (valueOfPart ("stageTwoCurve") == doctest::Approx (0.0f));
}
