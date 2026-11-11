// SPDX-License-Identifier: AGPL-3.0-or-later
// What devices' and modulators' editors show under the pointer of what a drag there moves (src/ui/devices/GraphDot.h; the
// owner, 2026-11-10: "it should be obvious that you are able to do this gesture"): the pointer says how the drag moves, the
// handle a press takes lights up, and the hint bar (the description) says what it does.
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Chorus.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/reverb/Reverb.h"
#include "devices/saturation/Exciter.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "devices/vinyl/Vinyl.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/ChorusEditor.h"
#include "ui/devices/DelayEditor.h"
#include "ui/devices/DrumSamplerEditor.h"
#include "ui/devices/ExciterEditor.h"
#include "ui/devices/FilterEditor.h"
#include "ui/devices/FlangerEditor.h"
#include "ui/devices/ModulatorEditor.h"
#include "ui/devices/PhaserEditor.h"
#include "ui/devices/ReverbEditor.h"
#include "ui/devices/SaturatorEditor.h"
#include "ui/devices/SynthEditor.h"
#include "ui/devices/VinylEditor.h"
#include "ui/devices/WavetableEditor.h"
#include "ui/theme/DesignTokens.h"

#include <string_view>

namespace
{
using Cursor = juce::MouseCursor;
using phazite::ui::GraphTint::dotRadius;

// One of Phazite's own devices on the song's first track, its instrument or an effect after it, which the song's session plays.
phazite::DeviceID addDeviceID (phazite::test::TestSong& song, std::string_view typeID, bool isInstrument = false)
{
    auto& s = *phazite::findSong (song.context);
    const auto state = phazite::makeOwnDeviceState (s, typeID);
    const auto id =
        isInstrument ? phazite::setInstrument (s, song.firstTrack, state) : phazite::insertDeviceState (s, song.firstTrack, state);
    REQUIRE (id.isValid());
    phazite::getSession (song.context)->update();
    return id;
}

phazite::DeviceHandle addDevice (phazite::test::TestSong& song, std::string_view typeID, bool isInstrument = false)
{
    return phazite::makeDeviceHandle (*phazite::findSong (song.context), addDeviceID (song, typeID, isInstrument));
}

juce::Image snapshot (juce::Component& c)
{
    return c.createComponentSnapshot (c.getLocalBounds());
}

// Where two pictures of a component differ: the smallest rectangle around the pixels that changed (empty if none did).
juce::Rectangle<int> getChangedArea (const juce::Image& before, const juce::Image& after)
{
    const juce::Image::BitmapData was (before, juce::Image::BitmapData::readOnly), now (after, juce::Image::BitmapData::readOnly);
    juce::Rectangle<int> changed;

    for (int y = 0; y < std::min (was.height, now.height); ++y)
        for (int x = 0; x < std::min (was.width, now.width); ++x)
            if (was.getPixelColour (x, y) != now.getPixelColour (x, y))
                changed = changed.getUnion ({ x, y, 1, 1 });

    return changed;
}

// A point of a component a little inside its corner furthest from a place.
juce::Point<int> getFarCorner (const juce::Component& c, juce::Point<float> from)
{
    return { from.x < (float) c.getWidth() * 0.5f ? c.getWidth() - 6 : 6, from.y < (float) c.getHeight() * 0.5f ? c.getHeight() - 6 : 6 };
}

// A graph whose dot a drag moves both ways, and a press anywhere puts under the pointer (GraphDot): over the dot, the grabbing
// hand, and the dot ringed a little further out than it is; elsewhere, the crosshair, and the dot as it was; and while a
// press drags it, the hand. The hint says so.
void checkDot (juce::Component& graph, juce::Point<float> dot)
{
    phazite::test::Mouse mouse (graph);
    const auto before = snapshot (graph);
    const auto away = getFarCorner (graph, dot);
    CHECK (graph.getDescription().contains ("Drag the dot, or press anywhere to put it there"));

    mouse.moveTo (away);
    CHECK (graph.getMouseCursor() == Cursor (Cursor::CrosshairCursor));
    CHECK (getChangedArea (before, snapshot (graph)).isEmpty());

    mouse.moveTo (dot.roundToInt());
    CHECK (graph.getMouseCursor() == Cursor (Cursor::DraggingHandCursor));
    const auto lit = getChangedArea (before, snapshot (graph));
    CHECK (lit.contains (dot.roundToInt()));
    CHECK (lit.getWidth() <= 2 * juce::roundToInt (dotRadius + 4.0f));
    CHECK (lit.getHeight() <= 2 * juce::roundToInt (dotRadius + 4.0f));

    mouse.moveTo (away);
    CHECK (graph.getMouseCursor() == Cursor (Cursor::CrosshairCursor));
    CHECK (getChangedArea (before, snapshot (graph)).isEmpty());

    mouse.press (away);
    CHECK (graph.getMouseCursor() == Cursor (Cursor::DraggingHandCursor));
    mouse.release();
}
} // namespace

TEST_CASE ("Over the filter's dot the pointer grabs it and the dot lights up; elsewhere it is the crosshair, where a press puts the dot")
{
    phazite::test::TestSong song;
    phazite::ui::FilterEditor editor (addDevice (song, phazite::devices::FilterEffect::typeID));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkDot (editor.getGraph(), editor.getDotPosition());
}

TEST_CASE ("Over the exciter's dot the pointer grabs it and the dot lights up; elsewhere it is the crosshair, where a press puts the dot")
{
    phazite::test::TestSong song;
    phazite::ui::ExciterEditor editor (addDevice (song, phazite::devices::Exciter::typeID));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkDot (editor.getGraph(), editor.getDotPosition());
}

TEST_CASE (
    "Over a synth's filter's dot the pointer grabs it and the dot lights up; elsewhere it is the crosshair, where a press puts the dot")
{
    // The synth's, the wavetable synth's and the FM synth's filter views are the same (SynthParts.h).
    phazite::test::TestSong song;
    phazite::ui::SynthEditor editor (addDevice (song, phazite::devices::Synth::typeID, true));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& filter = editor.getFilter();
    checkDot (filter, filter.getDotPosition());
}

TEST_CASE ("Over a step sequencer's steps the pointer is the crosshair, where a press sets a step, and the step under it lights up")
{
    phazite::test::TestSong song;
    const auto modulator = phazite::addModulator (song.context, song.firstTrack, phazite::ModulatorType::steps);
    phazite::ui::ModulatorEditor editor { song.context, modulator };
    phazite::test::TestWindow window { editor, editor.getWidth(), editor.getHeight() };
    auto* steps = phazite::test::findComponentWithTitle (editor, "Steps");
    REQUIRE (steps != nullptr);
    const auto count = (int) phazite::getSteps (song.context, modulator).size();
    REQUIRE (count >= 4);
    const auto stepWidth = steps->getWidth() / count;
    phazite::test::Mouse mouse (*steps);
    const auto before = snapshot (*steps);

    // Over the third step, high up: the crosshair, and that step ringed, not its neighbours.
    mouse.moveTo ({ 2 * stepWidth + stepWidth / 2, steps->getHeight() / 4 });
    CHECK (steps->getMouseCursor() == Cursor (Cursor::CrosshairCursor));
    const auto lit = getChangedArea (before, snapshot (*steps));
    REQUIRE_FALSE (lit.isEmpty());
    CHECK (lit.getCentreX() >= 2 * stepWidth);
    CHECK (lit.getCentreX() <= 3 * stepWidth + 2);
    CHECK (lit.getWidth() <= stepWidth + 8);
    CHECK (steps->getDescription().startsWith ("Click a step to set it as high as the pointer"));

    // Off them: as they were.
    mouse.moveTo ({ -50, -50 });
    CHECK (getChangedArea (before, snapshot (*steps)).isEmpty());
}

TEST_CASE ("Over the vinyl's record the pointer moves up and down, for the wow; over its band, sideways, the nearer end lit")
{
    phazite::test::TestSong song;
    phazite::ui::VinylEditor editor (addDevice (song, phazite::devices::Vinyl::typeID));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& record = editor.getRecord();
    phazite::test::Mouse mouse (record);
    const auto before = snapshot (record);

    // On the record: up and down, and the record ringed.
    const auto centre = editor.getRecordCentre().roundToInt();
    mouse.moveTo (centre);
    CHECK (record.getMouseCursor() == Cursor (Cursor::UpDownResizeCursor));
    const auto ringed = getChangedArea (before, snapshot (record));
    REQUIRE_FALSE (ringed.isEmpty());
    CHECK (ringed.contains (centre));
    CHECK (record.getDescription().startsWith ("Drag up or down for the wow"));

    // On the band, nearer its high end: sideways, and only that end lit.
    const auto band = editor.getBandArea();
    mouse.moveTo ({ band.getRight() - 4, band.getCentreY() });
    CHECK (record.getMouseCursor() == Cursor (Cursor::LeftRightResizeCursor));
    const auto end = getChangedArea (before, snapshot (record));
    REQUIRE_FALSE (end.isEmpty());
    CHECK (end.getX() > band.getCentreX());
    CHECK (band.contains (end.getCentre()));
    CHECK (record.getDescription().contains ("the high cut"));

    // Nearer its low end, that one.
    mouse.moveTo ({ band.getX() + 2, band.getCentreY() });
    CHECK (record.getMouseCursor() == Cursor (Cursor::LeftRightResizeCursor));
    CHECK (record.getDescription().contains ("the low cut"));
}

TEST_CASE ("Over the chorus's, the flanger's and the phaser's dots the pointer grabs them and they light up; elsewhere, the crosshair")
{
    phazite::test::TestSong song;

    {
        phazite::ui::ChorusEditor editor (addDevice (song, phazite::devices::Chorus::typeID));
        phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
        checkDot (editor.getPicture(), editor.getDotPosition());
    }

    {
        phazite::ui::FlangerEditor editor (addDevice (song, phazite::devices::Flanger::typeID));
        phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
        checkDot (editor.getPicture(), editor.getDotPosition());
    }

    {
        phazite::ui::PhaserEditor editor (addDevice (song, phazite::devices::Phaser::typeID));
        phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
        checkDot (editor.getPicture(), editor.getDotPosition());
    }
}

TEST_CASE ("Over the delay's echoes the pointer is the crosshair and the first echo lights up; over its band, the nearer end")
{
    phazite::test::TestSong song;
    phazite::ui::DelayEditor editor (addDevice (song, phazite::devices::Delay::typeID));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& echoes = editor.getEchoes();
    phazite::test::Mouse mouse (echoes);
    const auto before = snapshot (echoes);

    // Over the third echo: a press there would put the first echo under the pointer, so the first lights up.
    mouse.moveTo ((editor.getEchoPosition (3) - juce::Point<float> (0.0f, 10.0f)).roundToInt());
    CHECK (echoes.getMouseCursor() == Cursor (Cursor::CrosshairCursor));
    const auto first = getChangedArea (before, snapshot (echoes));
    REQUIRE_FALSE (first.isEmpty());
    CHECK (std::abs (first.getCentreX() - juce::roundToInt (editor.getEchoPosition (1).x)) <= 2);
    CHECK (echoes.getDescription().startsWith ("Press or drag to put the first echo under the pointer"));

    // Over the band, nearer its high end: the pointer moves sideways, and only that end lights up.
    const auto band = editor.getBandArea();
    mouse.moveTo ({ band.getX() + band.getWidth() * 3 / 4, band.getCentreY() });
    CHECK (echoes.getMouseCursor() == Cursor (Cursor::LeftRightResizeCursor));
    const auto end = getChangedArea (before, snapshot (echoes));
    REQUIRE_FALSE (end.isEmpty());
    CHECK (end.getX() > band.getCentreX());
    CHECK (band.contains (end.getCentre()));
    CHECK (echoes.getDescription().contains ("the high cut"));

    // Nearer its low end, that one.
    mouse.moveTo ({ band.getX() + 2, band.getCentreY() });
    CHECK (echoes.getMouseCursor() == Cursor (Cursor::LeftRightResizeCursor));
    CHECK (echoes.getDescription().contains ("the low cut"));
}

TEST_CASE ("Over the reverb's edges the pointer says which way each moves, the one a press takes lights up, and the hint says which")
{
    using phazite::devices::Reverb;
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = addDeviceID (song, Reverb::typeID);
    phazite::ui::ReverbEditor editor (phazite::makeDeviceHandle (s, id));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& graph = editor.getGraph();
    phazite::test::Mouse mouse (graph);
    const auto before = snapshot (graph);

    // The tail's end: sideways, and ringed.
    const auto decay = editor.getDecayHandle().roundToInt();
    mouse.moveTo (decay);
    CHECK (graph.getMouseCursor() == Cursor (Cursor::LeftRightResizeCursor));
    CHECK (getChangedArea (before, snapshot (graph)).contains (decay));
    CHECK (graph.getDescription().startsWith ("Drag sideways for the decay"));

    // The damping's corner: up and down.
    mouse.moveTo (editor.getDampingHandle().roundToInt());
    CHECK (graph.getMouseCursor() == Cursor (Cursor::UpDownResizeCursor));
    CHECK (graph.getDescription().startsWith ("Drag up or down for the damping"));

    // A longer pre-delay: its edge sideways, and before it, away from the cuts' lines, nothing that drags.
    phazite::setValueSetByHand (s, phazite::ParameterRef { id, "predelay" }, 250.0f);
    editor.refresh();
    const auto predelay = editor.getPredelayHandle().roundToInt();
    mouse.moveTo (predelay);
    CHECK (graph.getMouseCursor() == Cursor (Cursor::LeftRightResizeCursor));
    CHECK (graph.getDescription().startsWith ("Drag sideways for the pre-delay"));

    REQUIRE (predelay.x > 18);
    mouse.moveTo (predelay - juce::Point<int> (15, 0));
    CHECK (graph.getMouseCursor() == Cursor (Cursor::NormalCursor));
    CHECK (graph.getDescription().startsWith ("How the tail fades"));
}

TEST_CASE ("Over the saturator's curve the pointer points up and down and the curve lights up")
{
    phazite::test::TestSong song;
    phazite::ui::SaturatorEditor editor (addDevice (song, phazite::devices::Saturator::typeID));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();
    phazite::test::Mouse mouse (curve);
    const auto before = snapshot (curve);

    mouse.moveTo (curve.getLocalBounds().getCentre());
    CHECK (curve.getMouseCursor() == Cursor (Cursor::UpDownResizeCursor));
    CHECK_FALSE (getChangedArea (before, snapshot (curve)).isEmpty());

    // Off it, onto a shape's button: as it was.
    auto& button = editor.getShapeButton (0);
    phazite::test::Mouse (button).moveTo (button.getLocalBounds().getCentre());
    CHECK (getChangedArea (before, snapshot (curve)).isEmpty());
}

TEST_CASE ("Over a wavetable oscillator's table the pointer grabs it and the frame it plays lights up")
{
    phazite::test::TestSong song;
    phazite::ui::WavetableEditor editor (addDevice (song, phazite::devices::WavetableSynth::typeID, true));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& view = editor.getTableView (0);
    phazite::test::Mouse mouse (view);
    const auto before = snapshot (view);

    mouse.moveTo (view.getLocalBounds().getCentre());
    CHECK (view.getMouseCursor() == Cursor (Cursor::DraggingHandCursor));
    CHECK_FALSE (getChangedArea (before, snapshot (view)).isEmpty());

    // Off it, onto the button that names its table: as it was.
    auto& button = editor.getTableButton (0);
    phazite::test::Mouse (button).moveTo (button.getLocalBounds().getCentre());
    CHECK (getChangedArea (before, snapshot (view)).isEmpty());
}

TEST_CASE ("Over the drum sampler's sound the nearest handle lights up and moves sideways; over its map, the pads a click shows")
{
    using phazite::devices::DrumSampler;
    using Field = DrumSampler::Field;
    phazite::test::TestSong song;
    const auto device = addDevice (song, DrumSampler::typeID, true);
    auto* sampler = device.getProcessor<DrumSampler>();
    REQUIRE (sampler != nullptr);
    sampler->prepare (48000.0, 4800);
    phazite::ui::DrumSamplerEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // The map: a click there shows other pads, which it frames first; at its top, the highest sixteen.
    auto& map = editor.getPadMap();
    phazite::test::Mouse mapMouse (map);
    const auto mapBefore = snapshot (map);
    mapMouse.moveTo ({ map.getWidth() / 2, 2 });
    CHECK (map.getMouseCursor() == Cursor (Cursor::PointingHandCursor));
    const auto framed = getChangedArea (mapBefore, snapshot (map));
    REQUIRE_FALSE (framed.isEmpty());
    CHECK (framed.getBottom() <= map.getHeight() / 4);

    // The kick's sound, which plays whole: its hold's end at the right, at the top, lit, and the pointer moves it sideways.
    auto& waveform = editor.getWaveform();
    phazite::test::Mouse mouse (waveform);
    const auto before = snapshot (waveform);
    const auto holdX = juce::roundToInt (editor.getHandleX (Field::hold));
    mouse.moveTo ({ holdX - 3, 6 });
    CHECK (waveform.getMouseCursor() == Cursor (Cursor::LeftRightResizeCursor));
    const auto lit = getChangedArea (before, snapshot (waveform));
    REQUIRE_FALSE (lit.isEmpty());
    CHECK (lit.getX() <= holdX);
    CHECK (lit.getRight() >= holdX);
    CHECK (lit.getBottom() < waveform.getHeight() / 2);
    CHECK (waveform.getDescription().startsWith ("Drag sideways to move the hold's end"));

    // Anywhere, a press takes the nearest: halfway down at the left, the start.
    mouse.moveTo ({ juce::roundToInt (editor.getHandleX (Field::start)) + 8, waveform.getHeight() / 2 });
    CHECK (waveform.getMouseCursor() == Cursor (Cursor::LeftRightResizeCursor));
    CHECK (waveform.getDescription().startsWith ("Drag sideways to move where the sound starts"));

    // A pad without a sound has nothing to drag.
    editor.selectPad (100);
    mouse.moveTo (waveform.getLocalBounds().getCentre());
    CHECK (waveform.getMouseCursor() == Cursor (Cursor::NormalCursor));
}
