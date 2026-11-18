// SPDX-License-Identifier: AGPL-3.0-or-later
// How the graphs in the editors of Phazite's own devices take their handles (src/ui/devices/DeviceGraph.h,
// graph::HandleDrag), driven with the mouse: a press at a handle's edge changes nothing, and a drag moves it as far as the
// pointer goes (known bug 116); with a modulator on its parameter, the value set by hand moves by the drag alone (117);
// Escape in the middle of a drag, in every graph and on a knob's modulation ring, puts back what was there and leaves
// nothing to undo or redo (140). And the EQ's wheel over a dot (118), the analyzers' channels and taps (120), and the
// curves worked out only when what they show changes (130).
#include "AppScreen.h"
#include "EnginePlayback.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/age/Age.h"
#include "devices/core/SampleTap.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Motion.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/Stereo.h"
#include "devices/synth/Synth.h"
#include "devices/vocoder/Vocoder.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/automation/AutomationModel.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/components/DisplayTicker.h"
#include "ui/devices/AgeEditor.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/DelayEditor.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DeviceGraph.h"
#include "ui/devices/DrumSamplerEditor.h"
#include "ui/devices/DynamicsCurve.h"
#include "ui/devices/EqEditor.h"
#include "ui/devices/FilterEditor.h"
#include "ui/devices/LevelHistoryView.h"
#include "ui/devices/LimiterEditor.h"
#include "ui/devices/ModulatorEditor.h"
#include "ui/devices/MotionEditor.h"
#include "ui/devices/MultibandEditor.h"
#include "ui/devices/ReverbEditor.h"
#include "ui/devices/SamplerEditor.h"
#include "ui/devices/SaturatorEditor.h"
#include "ui/devices/StereoEditor.h"
#include "ui/devices/SynthEditor.h"
#include "ui/devices/SynthParts.h"
#include "ui/input/WindowKeys.h"
#include "ui/theme/DesignTokens.h"

#include <cmath>
#include <vector>

namespace
{
using phazite::ModulatorType;
using phazite::test::Mouse;
using phazite::test::SongPart;
using phazite::test::TestSong;
using phazite::test::TestWindow;

const auto escape = juce::KeyPress (juce::KeyPress::escapeKey);

phazite::Song& songOf (TestSong& song)
{
    return *phazite::findSong (song.context);
}

juce::String songText (TestSong& song)
{
    return phazite::test::writeSong (songOf (song), SongPart::undoable);
}

float valueOf (TestSong& song, phazite::DeviceID device, const char* id)
{
    return phazite::getValueSetByHand (songOf (song), phazite::ParameterRef { device, id });
}

// A device's editor, made as the app makes it, in a window of its size.
struct EditorScreen
{
    EditorScreen (TestSong& s, phazite::DeviceID id)
        : editor (phazite::ui::createDeviceEditor (phazite::getDeviceHandle (s.context, id)))
    {
        REQUIRE (editor != nullptr);
        window = std::make_unique<TestWindow> (*editor, editor->getWidth(), editor->getHeight());
    }

    juce::Component& find (const juce::String& title) const
    {
        auto* found = phazite::test::findComponentWithTitle (*editor, title);
        REQUIRE (found != nullptr);
        REQUIRE (found->isShowing());
        return *found;
    }

    std::unique_ptr<juce::Component> editor;
    std::unique_ptr<TestWindow> window;
};

// A drag on a graph from one place to another, Escape pressed in the middle of it, then on and let go: the song as it was,
// at Escape and after, and nothing to undo or redo, as in every editor of the time line (EditorDragTests.cpp).
void dragThenEscape (TestSong& song, TestWindow& window, juce::Component& graph, juce::Point<int> from, juce::Point<int> to)
{
    auto& s = songOf (song);
    s.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (s);
    const auto before = songText (song);

    undo.check ("a drag, then Escape",
                [&]
                {
                    Mouse mouse (graph);
                    mouse.press (from);
                    mouse.dragTo ((from + to) / 2);
                    mouse.dragTo (to);
                    CHECK (songText (song) != before); // the drag changed something
                    window.pressKey (escape);
                    CHECK (songText (song) == before);
                    mouse.dragTo (to + juce::Point<int> (15, 15)); // the drag does nothing more until the mouse goes up
                    mouse.release();
                    mouse.pause();
                });

    CHECK (songText (song) == before);
    CHECK_FALSE (s.getUndoManager().canUndo());
    CHECK_FALSE (s.getUndoManager().canRedo());
}

// A press and a release, without moving, at a place: the song as it was, nothing to undo.
void pressAndLetGo (TestSong& song, juce::Component& graph, juce::Point<int> at)
{
    auto& s = songOf (song);
    s.getUndoManager().clearUndoHistory();
    const auto before = songText (song);
    Mouse mouse (graph);
    mouse.press (at);
    mouse.release();
    mouse.pause();
    CHECK (songText (song) == before);
    CHECK_FALSE (s.getUndoManager().canUndo());
}

// A drag of a handle by so far, from a press at a place.
void dragBy (juce::Component& graph, juce::Point<int> from, juce::Point<int> by)
{
    Mouse mouse (graph);
    mouse.press (from);
    mouse.dragTo (from + by / 2);
    mouse.dragTo (from + by);
    mouse.release();
    mouse.pause();
}

// The mouse, its button down, moved less than a pixel, as a hand's tremor moves it: JUCE sends a drag for it, whose offset
// from the press is (0, 0) in whole pixels.
void tremble (juce::Component& graph, juce::Point<int> pressedAt)
{
    auto* peer = graph.getPeer();
    REQUIRE (peer != nullptr);
    const auto at = peer->getComponent().getLocalPoint (&graph, pressedAt.toFloat() + juce::Point<float> (0.3f, 0.2f));
    const auto mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
    juce::ModifierKeys::currentModifiers = mods;
    peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse, at, mods, juce::MouseInputSource::defaultPressure,
                            juce::MouseInputSource::defaultOrientation, juce::Time::currentTimeMillis() + 100000);
}

// The project saved, as Save saves it: from here on, a change is an unsaved change.
void save (TestSong& song)
{
    auto saved = false;
    song.save ([&saved] (bool done) { saved = done; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&saved] { return saved; }));
    REQUIRE_FALSE (song.getProject().hasUnsavedChanges());
}

// The commands run, by their IDs, as the registry hears them.
struct Invoked : public phazite::CommandRegistry::Listener
{
    void commandAdded (const phazite::Command&) override {}
    void commandInvoked (const phazite::Command& command) override { ids.add (command.id); }
    juce::StringArray ids;
};

// An LFO at 1 Hz on a device's parameter, played to the top of its cycle: what plays then is far from the value set by hand.
// An instrument's track plays a note meanwhile.
phazite::ModulatorID modulateAndPlay (TestSong& song, phazite::test::EnginePlayback& playback, const phazite::ParameterRef& parameter,
                                      float amount, bool withNote = false)
{
    song.setTempo (120.0);
    const auto clip = song.addMidiClip (song.firstTrack, 0.0, 16.0);

    if (withNote)
        song.addNote (clip, 60, 0.0, 16.0);

    const auto lfo = song.addModulator (song.firstTrack, ModulatorType::lfo);
    song.setModulatorParameter (lfo, "rateType", 0.0f); // in hertz, at 1 Hz
    song.modulate (lfo, parameter, amount);
    song.run ("transport.playStop");

    // To near the top or the bottom of its cycle (where it is in it as Play starts is the LFO's own: known bug 137).
    const auto handle = phazite::getParameter (song.context, parameter);
    const auto swing = amount * (handle.getRange().end - handle.getRange().start);

    for (int step = 0; step < 40 && std::abs (handle.getValue() - handle.getBaseValue()) < 0.6f * swing; ++step)
        playback.play (0.05);

    return lfo;
}
} // namespace

//==============================================================================
TEST_CASE ("A press at a graph's handle's edge changes nothing, and a drag moves the handle as far as the pointer goes (known bug 116)")
{
    TestSong song;

    SUBCASE ("An EQ band's dot")
    {
        const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
        phazite::ui::EqEditor editor (phazite::getDeviceHandle (song.context, eq));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto& graph = editor.getGraph();
        const auto edge = (editor.getDotPosition (3) + juce::Point<float> (phazite::ui::GraphTint::dotRadius, 0.0f)).roundToInt();

        pressAndLetGo (song, graph, edge);
        CHECK (valueOf (song, eq, "band4Frequency") == doctest::Approx (800.0f));

        // From its edge, 30 pixels right and 20 up: the dot moves as far, not to the pointer.
        dragBy (graph, edge, { 30, -20 });
        CHECK (valueOf (song, eq, "band4Frequency")
               == doctest::Approx (800.0 * std::pow (1000.0, 30.0 / graph.getWidth())).epsilon (0.002));
        CHECK (valueOf (song, eq, "band4Gain") == doctest::Approx (20.0 / graph.getHeight() * 48.0).epsilon (0.002));
    }

    SUBCASE ("A multiband's crossover")
    {
        const auto device = song.addEffect (song.firstTrack, phazite::devices::Multiband::typeID);
        phazite::ui::MultibandEditor editor (phazite::getDeviceHandle (song.context, device));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto& graph = editor.getBandsGraph();
        const auto low = valueOf (song, device, "lowFrequency");
        const auto x = editor.getBandsX (low);
        const auto edge = juce::Point<float> (x + 6.0f, (float) graph.getHeight() / 2.0f).roundToInt();

        // A press and the smallest of moves: nothing more than the move.
        pressAndLetGo (song, graph, edge);
        dragBy (graph, edge, { -40, 0 });
        CHECK (editor.getBandsX (valueOf (song, device, "lowFrequency")) == doctest::Approx (x - 40.0f).epsilon (0.002));
    }

    SUBCASE ("The reverb's pre-delay")
    {
        const auto device = song.addEffect (song.firstTrack, phazite::devices::Reverb::typeID);
        phazite::ui::ReverbEditor editor (phazite::getDeviceHandle (song.context, device));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto& graph = editor.getGraph();
        const auto predelay = valueOf (song, device, "predelay");
        const auto edge = (editor.getPredelayHandle() + juce::Point<float> (7.0f, 0.0f)).roundToInt();

        pressAndLetGo (song, graph, edge);
        CHECK (valueOf (song, device, "predelay") == doctest::Approx (predelay));

        // 20 pixels right: as many milliseconds more as 20 pixels show, at the scale the drag began with.
        const auto shown = editor.getShownSeconds();
        dragBy (graph, edge, { 20, 0 });
        CHECK (valueOf (song, device, "predelay")
               == doctest::Approx (predelay + 20.0 / (graph.getWidth() - 2.0) * shown * 1000.0).epsilon (0.002));
    }

    SUBCASE ("The delay's and Age's bands' ends")
    {
        for (const auto* typeID : { phazite::devices::Delay::typeID, phazite::devices::Age::typeID })
        {
            CAPTURE (typeID);
            const auto device = song.addEffect (song.firstTrack, typeID);
            const auto isDelay = juce::String (typeID) == phazite::devices::Delay::typeID;
            const auto editor = phazite::ui::createDeviceEditor (phazite::getDeviceHandle (song.context, device));
            TestWindow window (*editor, editor->getWidth(), editor->getHeight());
            auto* delay = dynamic_cast<phazite::ui::DelayEditor*> (editor.get());
            auto* age = dynamic_cast<phazite::ui::AgeEditor*> (editor.get());
            REQUIRE ((isDelay ? delay != nullptr : age != nullptr));
            auto& graph = isDelay ? delay->getEchoes() : age->getRecord();
            const auto band = (isDelay ? delay->getBandArea() : age->getBandArea()).toFloat();
            const phazite::ui::graph::FrequencyAxis frequencies { band };
            const auto highCut = valueOf (song, device, "highCut");
            const auto edge = juce::Point<float> (frequencies.xFor (highCut) - 3.0f, band.getCentreY()).roundToInt();

            pressAndLetGo (song, graph, edge);
            CHECK (valueOf (song, device, "highCut") == doctest::Approx (highCut));

            dragBy (graph, edge, { -30, 0 });
            CHECK (frequencies.xFor (valueOf (song, device, "highCut"))
                   == doctest::Approx (frequencies.xFor (highCut) - 30.0f).epsilon (0.002));
        }
    }
}

//==============================================================================
TEST_CASE ("A drag in a graph moves the value set by hand by the drag alone, whatever its modulation plays now (known bug 117)")
{
    TestSong song;
    phazite::test::EnginePlayback playback;

    SUBCASE ("A synth's filter's dot (graph::GraphDot)")
    {
        using phazite::devices::Synth;
        const auto synth = song.setInstrument (song.firstTrack, Synth::typeID);
        modulateAndPlay (song, playback, { synth, "resonance" }, 0.5f, true);
        const auto handle = phazite::getParameter (song.context, { synth, "resonance" });
        REQUIRE (std::abs (handle.getValue() - handle.getBaseValue()) > 0.1f);
        const auto before = handle.getBaseValue();
        const auto cutoff = valueOf (song, synth, "cutoff");

        phazite::ui::SynthEditor editor (phazite::getDeviceHandle (song.context, synth));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto& filter = editor.getFilter();
        juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow(); // the dot where its modulation plays it
        dragBy (filter, filter.getDotPosition().roundToInt(), { 0, -10 });
        CHECK (valueOf (song, synth, "resonance")
               == doctest::Approx (before + 10.0f / ((float) filter.getHeight() - 4.0f)).epsilon (0.002));
        CHECK (valueOf (song, synth, "cutoff") == doctest::Approx (cutoff).epsilon (0.002));
    }

    SUBCASE ("A wavetable's position")
    {
        using phazite::devices::WavetableSynth;
        const auto synth = song.setInstrument (song.firstTrack, WavetableSynth::typeID);
        modulateAndPlay (song, playback, { synth, "osc1Position" }, 0.5f, true);
        const auto handle = phazite::getParameter (song.context, { synth, "osc1Position" });
        REQUIRE (std::abs (handle.getValue() - handle.getBaseValue()) > 0.05f);
        const auto before = handle.getBaseValue();

        EditorScreen screen (song, synth);
        auto& table = screen.find ("Osc 1 Wavetable");
        dragBy (table, table.getLocalBounds().getCentre(), { 20, 0 });
        CHECK (valueOf (song, synth, "osc1Position") == doctest::Approx (before + 20.0f / (float) table.getWidth()).epsilon (0.002));
    }

    SUBCASE ("A saturator's drive")
    {
        using phazite::devices::Saturator;
        const auto device = song.addEffect (song.firstTrack, Saturator::typeID);
        modulateAndPlay (song, playback, { device, "drive" }, 0.5f);
        const auto handle = phazite::getParameter (song.context, { device, "drive" });
        REQUIRE (std::abs (handle.getValue() - handle.getBaseValue()) > 1.0f);
        const auto before = handle.getBaseValue();

        EditorScreen screen (song, device);
        auto& display = screen.find ("Saturation");
        dragBy (display, display.getLocalBounds().getCentre(), { 0, -2 });
        const auto after = valueOf (song, device, "drive");
        CHECK (after > before);
        CHECK (after - before < 2.0f); // two pixels' worth, not the modulation's swing
    }
}

//==============================================================================
TEST_CASE ("Escape in the middle of a drag in a device's graph puts back what was there, leaving nothing to undo or redo (known bug 140)")
{
    TestSong song;

    SUBCASE ("The EQ, the multiband, the reverb and the filter")
    {
        const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
        {
            phazite::ui::EqEditor editor (phazite::getDeviceHandle (song.context, eq));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            const auto dot = editor.getDotPosition (3).roundToInt();
            dragThenEscape (song, window, editor.getGraph(), dot, dot + juce::Point<int> (40, -30));

            // A dynamic band's handle, which drags its Dynamic.
            song.setParameter (eq, "band4Dynamic", -6.0f);
            editor.refresh();
            const auto handle = editor.getDynamicHandlePosition (3);
            REQUIRE (handle.has_value());
            dragThenEscape (song, window, editor.getGraph(), handle->roundToInt(), handle->roundToInt() + juce::Point<int> (0, -30));
        }

        const auto multiband = song.addEffect (song.firstTrack, phazite::devices::Multiband::typeID);
        {
            phazite::ui::MultibandEditor editor (phazite::getDeviceHandle (song.context, multiband));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            auto& graph = editor.getBandsGraph();
            const auto crossover =
                juce::Point<float> (editor.getBandsX (valueOf (song, multiband, "lowFrequency")), graph.getHeight() / 2.0f);
            dragThenEscape (song, window, graph, crossover.roundToInt(), crossover.roundToInt() + juce::Point<int> (40, 0));
            const auto level = juce::Point<float> (editor.getBandsX (1000.0), editor.getBandsY (0.0f)).roundToInt();
            dragThenEscape (song, window, graph, level, level - juce::Point<int> (0, 30));
            phazite::test::checkChildrenApart (editor);
        }

        const auto reverb = song.addEffect (song.firstTrack, phazite::devices::Reverb::typeID);
        {
            phazite::ui::ReverbEditor editor (phazite::getDeviceHandle (song.context, reverb));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            const auto gap = editor.getPredelayHandle().roundToInt();
            dragThenEscape (song, window, editor.getGraph(), gap, gap + juce::Point<int> (30, 0));
            const auto end = editor.getDecayHandle().roundToInt();
            dragThenEscape (song, window, editor.getGraph(), end, end - juce::Point<int> (30, 0));
        }

        const auto filter = song.addEffect (song.firstTrack, phazite::devices::FilterEffect::typeID);
        {
            phazite::ui::FilterEditor editor (phazite::getDeviceHandle (song.context, filter));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            auto& graph = editor.getGraph();
            const auto dot = editor.getDotPosition().roundToInt();
            const auto middle = graph.getLocalBounds().getCentre();
            dragThenEscape (song, window, graph, dot, middle);        // the dot taken
            dragThenEscape (song, window, graph, middle, middle / 2); // put under the pointer as the press comes
        }
    }

    SUBCASE ("The delay, Age, Stereo, the saturator, the vocoder, the compressor and the limiter")
    {
        const auto delay = song.addEffect (song.firstTrack, phazite::devices::Delay::typeID);
        {
            phazite::ui::DelayEditor editor (phazite::getDeviceHandle (song.context, delay));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            const auto first = editor.getEchoPosition (1).roundToInt(), second = editor.getEchoPosition (2).roundToInt();
            dragThenEscape (song, window, editor.getEchoes(), first - juce::Point<int> (0, 10), second - juce::Point<int> (0, 60));
            const auto band = editor.getBandArea();
            const auto highEnd = juce::Point<int> (band.getX() + band.getWidth() * 3 / 4, band.getCentreY());
            dragThenEscape (song, window, editor.getEchoes(), highEnd, highEnd - juce::Point<int> (30, 0));
        }

        const auto age = song.addEffect (song.firstTrack, phazite::devices::Age::typeID);
        {
            phazite::ui::AgeEditor editor (phazite::getDeviceHandle (song.context, age));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            const auto centre = editor.getRecordCentre().roundToInt();
            dragThenEscape (song, window, editor.getRecord(), centre, centre - juce::Point<int> (0, 40));
        }

        const auto stereo = song.addEffect (song.firstTrack, phazite::devices::Stereo::typeID);
        {
            phazite::ui::StereoEditor editor (phazite::getDeviceHandle (song.context, stereo));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            const auto centre = editor.getDiscCentre();
            dragThenEscape (song, window, editor.getImage(), (centre + juce::Point<float> (0.0f, -40.0f)).roundToInt(),
                            (centre + juce::Point<float> (-40.0f, -5.0f)).roundToInt());
        }

        for (const auto& [typeID, title] : { std::pair { phazite::devices::Saturator::typeID, "Saturation" },
                                             std::pair { phazite::devices::Vocoder::typeID, "Vocoder bands" } })
        {
            CAPTURE (typeID);
            EditorScreen screen (song, song.addEffect (song.firstTrack, typeID));
            auto& graph = screen.find (title);
            const auto centre = graph.getLocalBounds().getCentre();
            dragThenEscape (song, *screen.window, graph, centre, centre + juce::Point<int> (40, -30));
        }

        // The saturator's band's edges.
        const auto saturator = song.addEffect (song.firstTrack, phazite::devices::Saturator::typeID);
        {
            phazite::ui::SaturatorEditor editor (phazite::getDeviceHandle (song.context, saturator));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            auto& view = editor.getBandView();
            const auto y = view.getHeight() / 2;
            const juce::Point<int> from (juce::roundToInt (editor.getEdgeX (false)), y),
                to (juce::roundToInt (editor.getEdgeX (true)) - 1, y);
            dragThenEscape (song, window, view, from, from - juce::Point<int> (30, 0));
            dragThenEscape (song, window, view, to, to - juce::Point<int> (40, 0));

            // From dragged up far enough to push To ahead of it, then Escape: both as they were.
            phazite::setValueSetByHand (songOf (song), { saturator, "bandHigh" }, 8000.0f);
            juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
            songOf (song).getUndoManager().clearUndoHistory();
            const auto beforePush = songText (song);
            const juce::Point<int> pushing (juce::roundToInt (editor.getEdgeX (false)), y);
            Mouse mouse (view);
            mouse.press (pushing);
            mouse.dragTo (pushing + juce::Point<int> (40, 0));
            mouse.dragTo (pushing + juce::Point<int> (80, 0));
            CHECK (valueOf (song, saturator, "bandHigh") > 8000.0f); // pushed
            window.pressKey (escape);
            CHECK (valueOf (song, saturator, "bandHigh") == 8000.0f);
            CHECK (valueOf (song, saturator, "bandLow") == 3000.0f);
            mouse.release();
            CHECK (songText (song) == beforePush);
            CHECK_FALSE (songOf (song).getUndoManager().canUndo());
            CHECK_FALSE (songOf (song).getUndoManager().canRedo());
        }

        const auto compressor = song.addEffect (song.firstTrack, phazite::devices::Compressor::typeID);
        {
            phazite::ui::CompressorEditor editor (phazite::getDeviceHandle (song.context, compressor));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            auto& curve = editor.getCurve();
            const auto corner = curve.getPointPosition (1).roundToInt();
            dragThenEscape (song, window, curve, corner, corner + juce::Point<int> (10, 20));
            REQUIRE (curve.getTensionPosition (2).has_value());
            const auto handle = curve.getTensionPosition (2)->roundToInt();
            dragThenEscape (song, window, curve, handle, handle - juce::Point<int> (0, 20));
        }

        const auto limiter = song.addEffect (song.firstTrack, phazite::devices::Limiter::typeID);
        {
            phazite::ui::LimiterEditor editor (phazite::getDeviceHandle (song.context, limiter));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            auto& history = editor.getDisplay().getHistory();
            REQUIRE (history.isShowing());
            const auto line =
                juce::Point<float> ((float) history.getWidth() / 2.0f, history.getYForLevel (valueOf (song, limiter, "ceiling")));
            dragThenEscape (song, window, history, line.roundToInt(), line.roundToInt() + juce::Point<int> (0, 30));
        }
    }

    SUBCASE ("A synth's envelope, LFO and filter, a wavetable and a sampler")
    {
        using Point = phazite::ui::EnvelopeView::Point;
        const auto synth = song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        {
            EditorScreen screen (song, synth);
            auto* envelope = dynamic_cast<phazite::ui::EnvelopeView*> (&screen.find ("Amp envelope"));
            REQUIRE (envelope != nullptr);
            const auto corner = envelope->getPointPosition (Point::decay).roundToInt();
            dragThenEscape (song, *screen.window, *envelope, corner, corner + juce::Point<int> (20, 10));
            REQUIRE (envelope->getBendPosition (Point::attack).has_value());
            const auto bend = envelope->getBendPosition (Point::attack)->roundToInt();
            dragThenEscape (song, *screen.window, *envelope, bend, bend - juce::Point<int> (0, 10));

            const auto lfos = phazite::test::findAll<phazite::ui::LfoView> (*screen.editor);
            REQUIRE (lfos.size() == 1);
            const auto centre = lfos[0]->getLocalBounds().getCentre();
            dragThenEscape (song, *screen.window, *lfos[0], centre, centre - juce::Point<int> (0, 20));

            const auto filters = phazite::test::findAll<phazite::ui::FilterView> (*screen.editor);
            REQUIRE (! filters.isEmpty());
            REQUIRE (filters[0]->isShowing());
            const auto dot = filters[0]->getDotPosition().roundToInt();
            dragThenEscape (song, *screen.window, *filters[0], dot, filters[0]->getLocalBounds().getCentre());
        }

        const auto wavetable = song.setInstrument (song.firstTrack, phazite::devices::WavetableSynth::typeID);
        {
            EditorScreen screen (song, wavetable);
            auto& table = screen.find ("Osc 1 Wavetable");
            const auto centre = table.getLocalBounds().getCentre();
            dragThenEscape (song, *screen.window, table, centre, centre + juce::Point<int> (30, 0));
        }

        const auto sampler = song.setInstrument (song.firstTrack, phazite::devices::Sampler::typeID);
        {
            phazite::ui::SamplerEditor editor (phazite::getDeviceHandle (song.context, sampler));
            TestWindow window (editor, editor.getWidth(), editor.getHeight());
            auto& view = editor.getSampleView();
            const auto from =
                juce::Point<float> (editor.getMarkerX (phazite::devices::Sampler::start), view.getHeight() / 2.0f).roundToInt();
            dragThenEscape (song, window, view, from, from + juce::Point<int> (view.getWidth() / 4, 0));
        }
    }

    SUBCASE ("A breakpoint envelope's point and a step sequencer's steps")
    {
        const auto breakpoints = song.addModulator (song.firstTrack, ModulatorType::breakpoints);
        {
            phazite::ui::ModulatorEditor editor { song.context, breakpoints };
            TestWindow window { editor, editor.getWidth(), editor.getHeight() };
            auto* graph = phazite::test::findComponentWithTitle (editor, "Breakpoints");
            REQUIRE (graph != nullptr);
            const auto area = graph->getLocalBounds().toFloat().reduced (phazite::ui::GraphTint::handleRadius * 1.5f + 1.0f);
            const auto start =
                juce::Point<float> (area.getX(),
                                    area.getBottom()
                                        - phazite::getModulatorParameter (song.context, breakpoints, "stageZeroValue").getValue()
                                              * area.getHeight())
                    .roundToInt();
            dragThenEscape (song, window, *graph, start, start + juce::Point<int> (0, start.y > area.getCentreY() ? -20 : 20));
        }

        const auto steps = song.addModulator (song.firstTrack, ModulatorType::steps);
        {
            phazite::ui::ModulatorEditor editor { song.context, steps };
            TestWindow window { editor, editor.getWidth(), editor.getHeight() };
            auto* view = phazite::test::findComponentWithTitle (editor, "Steps");
            REQUIRE (view != nullptr);
            dragThenEscape (song, window, *view, { 6, 2 }, { view->getWidth() / 2, 2 });
        }
    }
}

TEST_CASE (
    "Escape in the middle of a drag of a knob's modulation ring puts the amount back, leaving nothing to undo or redo (known bug 140)")
{
    TestSong song;
    const auto compressor = song.addEffect (song.firstTrack, phazite::devices::Compressor::typeID);
    auto handle = phazite::getParameter (song.context, { compressor, "makeup" });
    handle.setValue (6.0f); // the middle
    const auto lfo = phazite::modulateWithNew (handle, ModulatorType::lfo, 0.25f);
    REQUIRE (lfo.isValid());

    phazite::ui::Knob knob { "Makeup", 0.0 };
    phazite::ui::ParameterAttachment attachment { knob };
    attachment.setParameter (phazite::getParameter (song.context, { compressor, "makeup" }));
    attachment.update();
    REQUIRE (knob.getModulation().has_value());
    TestWindow window { knob, 64, 64 };

    const juce::Point<int> ring (32, 2);
    dragThenEscape (song, window, knob, ring, ring - juce::Point<int> (0, 40));
    REQUIRE (phazite::getModulations (handle).size() == 1);
    CHECK (phazite::getModulations (handle)[0].amount == doctest::Approx (0.25f));

    // And a drag let go is one step, as before.
    Mouse mouse (knob);
    mouse.press (ring);
    mouse.dragTo (ring - juce::Point<int> (0, 40));
    mouse.release();
    CHECK (phazite::getModulations (handle)[0].amount > 0.3f);
    CHECK (phazite::describeUndo (&songOf (song)) == juce::String ("Undo Change Modulation"));
}

//==============================================================================
TEST_CASE (
    "The wheel over an EQ band's dot turns its Q as one undo step until it rests; elsewhere on the graph it turns none (known bug 118)")
{
    TestSong song;
    const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
    phazite::ui::EqEditor editor (phazite::getDeviceHandle (song.context, eq));
    TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& s = songOf (song);
    s.getUndoManager().clearUndoHistory();
    Mouse mouse (editor.getGraph());

    // Away from every dot, with band 3 selected: nothing turns.
    editor.selectBand (2);
    std::vector<float> qs;

    for (int band = 1; band <= phazite::devices::Equalizer::numBands; ++band)
        qs.push_back (valueOf (song, eq, ("band" + juce::String (band) + "Q").toRawUTF8()));

    mouse.wheel ({ 2, 2 }, 0.5f);
    CHECK_FALSE (s.getUndoManager().canUndo());

    for (int band = 1; band <= phazite::devices::Equalizer::numBands; ++band)
        CHECK (valueOf (song, eq, ("band" + juce::String (band) + "Q").toRawUTF8()) == doctest::Approx (qs[(size_t) band - 1]));

    // Three notches over band 4's dot: one step, which takes all three back.
    const auto dot = editor.getDotPosition (3).roundToInt();
    mouse.wheel (dot, 0.5f);
    mouse.wheel (dot, 0.5f);
    mouse.wheel (dot, 0.5f);
    CHECK (valueOf (song, eq, "band4Q") == doctest::Approx (qs[3] * 8.0f).epsilon (0.01));
    REQUIRE (phazite::test::runTimersUntil ([&s] { return ! phazite::Gesture::isInProgress (s); }));
    phazite::undo (s);
    CHECK (valueOf (song, eq, "band4Q") == doctest::Approx (qs[3]));
    CHECK_FALSE (s.getUndoManager().canUndo());
}

//==============================================================================
TEST_CASE ("An analyzer shows a sound panned hard, or out of phase between the channels, as loud as one in the middle (known bug 120)")
{
    // A 1.5 kHz sine at -6 dBFS (on one of the analysis's frequencies), in both channels alike, in the left alone, and in both
    // with the right upside down; refreshed 30 times a second, as the display refreshes. Each reads at its level, less the
    // trace's tilt.
    constexpr double hertz = 1500.0;
    const auto tilt = phazite::ui::graph::SpectrumTrace::tiltPerOctave * (float) std::log2 (hertz / 1000.0);

    for (const auto& [left, right] : { std::pair { 1.0f, 1.0f }, std::pair { 1.0f, 0.0f }, std::pair { 1.0f, -1.0f } })
    {
        CAPTURE (left);
        CAPTURE (right);
        phazite::devices::StereoTap tap;
        phazite::ui::graph::SpectrumTrace trace;
        trace.setSampleRate (48000.0);
        trace.pull (tap); // it listens from here on
        std::vector<float> lefts (1600), rights (1600);

        for (int refresh = 0; refresh < 30; ++refresh)
        {
            for (size_t i = 0; i < lefts.size(); ++i)
            {
                const auto sine =
                    0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hertz * (double) (refresh * 1600 + (int) i) / 48000.0);
                lefts[i] = left * sine;
                rights[i] = right * sine;
            }

            tap.push (lefts.data(), rights.data(), (int) lefts.size());
            trace.pull (tap);
            trace.update();
        }

        CHECK (trace.getLevelAt (hertz) - tilt == doctest::Approx (-6.02f).epsilon (0.03));
    }
}

TEST_CASE (
    "The devices' taps hold a quarter of a second at 192 kHz, each channel apart, so that a refresh that comes late loses nothing (known bug 120)")
{
    constexpr int quarter = 48000; // a quarter of a second at 192 kHz
    std::vector<float> lefts (quarter), rights (quarter);

    for (int i = 0; i < quarter; ++i)
    {
        lefts[(size_t) i] = (float) i;
        rights[(size_t) i] = (float) -i;
    }

    // The scopes' taps, and a device's own (an EQ's), which is made as a display first listens.
    phazite::devices::StereoTap stereo;
    stereo.setListening (true);
    phazite::devices::SampleTap own;
    CHECK_FALSE (own.isListening());
    own.push (lefts.data(), rights.data(), 480); // nobody listens: nothing kept
    own.setListening (true);
    CHECK (own.isListening());

    for (int at = 0; at < quarter; at += 480)
    {
        stereo.push (lefts.data() + at, rights.data() + at, 480);
        own.push (lefts.data() + at, rights.data() + at, 480);
    }

    std::vector<float> pulledLeft (quarter + 100), pulledRight (quarter + 100);
    CHECK (stereo.pull (pulledLeft.data(), pulledRight.data(), quarter + 100) == quarter);
    CHECK (pulledLeft[quarter - 1] == (float) (quarter - 1));
    CHECK (pulledRight[quarter - 1] == (float) -(quarter - 1));
    CHECK (own.pull (pulledLeft.data(), pulledRight.data(), quarter + 100) == quarter);
    CHECK (pulledRight[1000] == -1000.0f);

    // Or the two channels' average, for a display that wants one (Age's pitch).
    own.push (lefts.data() + 10, rights.data(), 1);
    CHECK (own.pull (pulledLeft.data(), 10) == 1);
    CHECK (pulledLeft[0] == 5.0f);
}

//==============================================================================
TEST_CASE ("The EQ's curves and the reverb's tail are worked out once for each change, not at each repaint (known bug 130)")
{
    TestSong song;

    SUBCASE ("The EQ")
    {
        const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
        phazite::ui::EqEditor editor (phazite::getDeviceHandle (song.context, eq));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto& graph = editor.getGraph();
        graph.createComponentSnapshot (graph.getLocalBounds());
        const auto once = editor.getCurvesWorkedOut();
        CHECK (once >= 1);

        // Repainted as its analyzer moves, 30 times a second: the same curves.
        for (int i = 0; i < 10; ++i)
        {
            editor.refresh();
            graph.createComponentSnapshot (graph.getLocalBounds());
        }

        CHECK (editor.getCurvesWorkedOut() == once);

        // A band's gain changed: once more.
        phazite::getParameter (song.context, { eq, "band4Gain" }).setValue (6.0f);
        editor.refresh();
        graph.createComponentSnapshot (graph.getLocalBounds());
        graph.createComponentSnapshot (graph.getLocalBounds());
        CHECK (editor.getCurvesWorkedOut() == once + 1);
    }

    SUBCASE ("The reverb")
    {
        const auto reverb = song.addEffect (song.firstTrack, phazite::devices::Reverb::typeID);
        phazite::ui::ReverbEditor editor (phazite::getDeviceHandle (song.context, reverb));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto& graph = editor.getGraph();
        const auto once = editor.getShapesWorkedOut();
        CHECK (once >= 1);

        for (int i = 0; i < 10; ++i)
        {
            editor.refresh();
            graph.createComponentSnapshot (graph.getLocalBounds());
        }

        CHECK (editor.getShapesWorkedOut() == once);
        phazite::getParameter (song.context, { reverb, "decay" }).setValue (5.0f);
        editor.refresh();
        editor.refresh();
        CHECK (editor.getShapesWorkedOut() == once + 1);
    }
}

//==============================================================================
// The review of the fixes of known bugs 116, 117 and 140 (2026-11-15).

TEST_CASE ("A drag begun just after a turn of the same graph's wheel is a step of its own, which Escape takes back alone; and the wheel "
           "turns nothing while a drag is taken")
{
    TestSong song;
    auto& s = songOf (song);
    const auto notch = 0.5f;

    SUBCASE ("The filter: the wheel turns the drive, the dot drags the cutoff and the resonance")
    {
        const auto filter = song.addEffect (song.firstTrack, phazite::devices::FilterEffect::typeID);
        phazite::ui::FilterEditor editor (phazite::getDeviceHandle (song.context, filter));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto& graph = editor.getGraph();
        const auto cutoff = valueOf (song, filter, "cutoff"), resonance = valueOf (song, filter, "resonance");
        const auto drive = valueOf (song, filter, "drive");

        for (const auto escapes : { true, false })
        {
            CAPTURE (escapes);
            s.getUndoManager().clearUndoHistory();
            Mouse mouse (graph);
            const auto dot = editor.getDotPosition().roundToInt();
            mouse.wheel (graph.getLocalBounds().getCentre(), notch); // its step still open, for half a second
            const auto turned = valueOf (song, filter, "drive");
            REQUIRE (turned > drive);

            mouse.press (dot);
            mouse.dragTo (dot + juce::Point<int> (-30, 20));
            mouse.wheel (dot + juce::Point<int> (-30, 20), notch); // turns nothing during the drag
            CHECK (valueOf (song, filter, "drive") == turned);

            if (escapes)
                window.pressKey (escape);

            mouse.release();
            mouse.pause();

            if (escapes)
            {
                // The dot as it was; the drive's turn is a step of its own, which Undo takes back.
                CHECK (valueOf (song, filter, "cutoff") == cutoff);
                CHECK (valueOf (song, filter, "resonance") == resonance);
                CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Drive"));
                CHECK_FALSE (s.getUndoManager().canRedo());
            }
            else
            {
                // Two steps, each named: the drag's, then the wheel's.
                CHECK (valueOf (song, filter, "cutoff") < cutoff);
                CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Cutoff"));
                phazite::undo (s);
                CHECK (valueOf (song, filter, "cutoff") == cutoff);
                CHECK (valueOf (song, filter, "drive") == turned);
                CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Drive"));
            }

            phazite::undo (s);
            CHECK (valueOf (song, filter, "drive") == drive);
            CHECK_FALSE (s.getUndoManager().canUndo());
        }
    }

    SUBCASE ("Stereo: the wheel and the drag move the same width")
    {
        const auto device = song.addEffect (song.firstTrack, phazite::devices::Stereo::typeID);
        phazite::ui::StereoEditor editor (phazite::getDeviceHandle (song.context, device));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        const auto centre = editor.getDiscCentre();
        const auto from = (centre + juce::Point<float> (40.0f, -40.0f)).roundToInt();
        const auto to = (centre + juce::Point<float> (0.0f, -40.0f)).roundToInt();
        const auto width = valueOf (song, device, "width");

        for (const auto escapes : { true, false })
        {
            CAPTURE (escapes);
            s.getUndoManager().clearUndoHistory();
            Mouse mouse (editor.getImage());
            mouse.wheel (from, -notch);
            const auto turned = valueOf (song, device, "width");
            REQUIRE (turned < width);

            mouse.press (from);
            mouse.dragTo (to);
            mouse.wheel (to, notch); // turns nothing during the drag
            const auto dragged = valueOf (song, device, "width");
            CHECK (dragged < turned - 0.5f);

            if (escapes)
                window.pressKey (escape);

            mouse.release();
            mouse.pause();
            CHECK (valueOf (song, device, "width") == (escapes ? turned : dragged));

            if (! escapes)
            {
                CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Width"));
                phazite::undo (s);
                CHECK (valueOf (song, device, "width") == turned);
            }

            CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Width"));
            phazite::undo (s);
            CHECK (valueOf (song, device, "width") == width);
            CHECK_FALSE (s.getUndoManager().canUndo());
        }
    }
}

TEST_CASE ("A drag along one of a graph's axes leaves the other's value as it was, bit for bit, and a tremor under a pixel writes nothing")
{
    TestSong song;
    auto& s = songOf (song);
    const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
    phazite::getParameter (song.context, { eq, "band4Gain" }).setValue (3.0f);
    phazite::ui::EqEditor editor (phazite::getDeviceHandle (song.context, eq));
    TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& graph = editor.getGraph();
    s.getUndoManager().clearUndoHistory();

    // Sideways alone: the frequency moves, the gain is 3 exactly, and the project differs by the frequency alone.
    const auto before = songText (song);
    dragBy (graph, editor.getDotPosition (3).roundToInt(), { 30, 0 });
    CHECK (valueOf (song, eq, "band4Frequency") > 900.0f);
    CHECK (valueOf (song, eq, "band4Gain") == 3.0f);
    const auto differences = phazite::test::describeSongDifferences (before, songText (song));
    CAPTURE (differences.joinIntoString ("\n"));
    CHECK (differences.size() == 1);

    // Pressed, trembled under a pixel and let go: no step, and nothing to save.
    save (song);
    s.getUndoManager().clearUndoHistory();
    const auto saved = songText (song);
    const auto dot = editor.getDotPosition (3).roundToInt();
    Mouse mouse (graph);
    mouse.press (dot);
    tremble (graph, dot);
    mouse.release();
    mouse.pause();
    CHECK (songText (song) == saved);
    CHECK_FALSE (s.getUndoManager().canUndo());
    CHECK_FALSE (song.getProject().hasUnsavedChanges());

    // The filter's dot, through the dot such graphs share (GraphDot), whose axes go through pixels in floats, where a value
    // doesn't come back exactly: sideways, the resonance as it was, exactly; and a tremor, nothing.
    const auto filter = song.addEffect (song.firstTrack, phazite::devices::FilterEffect::typeID);
    phazite::ui::FilterEditor filterEditor (phazite::getDeviceHandle (song.context, filter));
    TestWindow filterWindow (filterEditor, filterEditor.getWidth(), filterEditor.getHeight());
    auto& filterGraph = filterEditor.getGraph();

    // A resonance that its place up the graph doesn't give back exactly (the graph's axis: its bounds, a pixel in).
    const auto area = filterGraph.getLocalBounds().toFloat().reduced (1.0f);
    auto resonance = 0.1f;

    for (auto r = 0.11f; r < 0.99f; r += 0.01f)
        if (const auto y = area.getBottom() - r * area.getHeight(); ! juce::exactlyEqual ((area.getBottom() - y) / area.getHeight(), r))
        {
            resonance = r;
            break;
        }

    REQUIRE_FALSE (juce::exactlyEqual (resonance, 0.1f));
    phazite::getParameter (song.context, { filter, "resonance" }).setValue (resonance);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow(); // the dot where it is now
    dragBy (filterGraph, filterEditor.getDotPosition().roundToInt(), { -25, 0 });
    CHECK (valueOf (song, filter, "resonance") == resonance);
    phazite::getParameter (song.context, { filter, "resonance" }).setValue (resonance);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();

    save (song);
    s.getUndoManager().clearUndoHistory();
    const auto filterSaved = songText (song);
    const auto filterDot = filterEditor.getDotPosition().roundToInt();
    Mouse filterMouse (filterGraph);
    filterMouse.press (filterDot);
    tremble (filterGraph, filterDot);
    filterMouse.release();
    filterMouse.pause();
    CHECK (songText (song) == filterSaved);
    CHECK_FALSE (s.getUndoManager().canUndo());
    CHECK_FALSE (song.getProject().hasUnsavedChanges());
}

TEST_CASE ("The delay's first echo moves from its time set by hand as far as the pointer, a press changing nothing, synced or free "
           "(known bug 116)")
{
    using phazite::devices::Delay;
    TestSong song;
    auto& s = songOf (song);
    const auto delay = song.addEffect (song.firstTrack, Delay::typeID);
    phazite::ui::DelayEditor editor (phazite::getDeviceHandle (song.context, delay));
    TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& echoes = editor.getEchoes();
    const auto step = editor.getEchoPosition (1).x - editor.getEchoPosition (0).x;
    const auto onEcho = (editor.getEchoPosition (1) - juce::Point<float> (3.0f, 10.0f)).roundToInt();

    // A press a few pixels off the echo, and let go: nothing.
    pressAndLetGo (song, echoes, onEcho);

    // Synced, a dotted eighth: dragged a pixel at a time as far right as the echoes are apart, twice as long, a dotted quarter.
    REQUIRE (valueOf (song, delay, "note") == 6.0f); // 1/8D
    const auto feedback = valueOf (song, delay, "feedback");
    Mouse mouse (echoes);
    mouse.press (onEcho);

    for (int x = 1; x <= juce::roundToInt (step); ++x)
        mouse.dragTo (onEcho + juce::Point<int> (x, 0));

    CHECK (valueOf (song, delay, "note") == 9.0f); // 1/4D, not one note value more for each event
    CHECK (valueOf (song, delay, "feedback") == feedback);
    mouse.release();
    mouse.pause();
    phazite::undo (s);

    // Free: 10 pixels right of a step's worth, the time in proportion, from 375 ms; 20 up, the feedback up as far.
    phazite::getParameter (song.context, { delay, "sync" }).setValue (0.0f);
    const auto milliseconds = valueOf (song, delay, "time");
    mouse.press (onEcho);

    for (int x = 1; x <= 10; ++x)
        mouse.dragTo (onEcho + juce::Point<int> (x, -2 * x));

    mouse.release();
    mouse.pause();
    CHECK (valueOf (song, delay, "time") == doctest::Approx (milliseconds * (step + 10.0f) / step).epsilon (0.002));
    CHECK (valueOf (song, delay, "feedback") > feedback);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Time"));
}

TEST_CASE ("The delay's first echo dragged while an LFO moves its time moves the time set by hand by the drag alone (known bug 117)")
{
    using phazite::devices::Delay;
    TestSong song;
    phazite::test::EnginePlayback playback;
    const auto delay = song.addEffect (song.firstTrack, Delay::typeID);
    song.setParameter (delay, "sync", 0.0f);
    song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID); // a sound into it, so that it doesn't sleep
    modulateAndPlay (song, playback, { delay, "time" }, 0.25f, true);
    const auto handle = phazite::getParameter (song.context, { delay, "time" });
    REQUIRE (std::abs (handle.getValue() - handle.getBaseValue()) > 50.0f);
    const auto before = handle.getBaseValue();

    phazite::ui::DelayEditor editor (phazite::getDeviceHandle (song.context, delay));
    TestWindow window (editor, editor.getWidth(), editor.getHeight());
    const auto step = editor.getEchoPosition (1).x - editor.getEchoPosition (0).x;
    dragBy (editor.getEchoes(), (editor.getEchoPosition (1) - juce::Point<float> (0.0f, 10.0f)).roundToInt(), { 10, 0 });
    CHECK (valueOf (song, delay, "time") == doctest::Approx (before * (step + 10.0f) / step).epsilon (0.002));
}

TEST_CASE ("A graph's handle held still while the song plays grabs its parameter in Touch, as a knob held still does")
{
    TestSong song;
    song.setTempo (120.0);
    phazite::test::EnginePlayback playback;
    const auto synth = song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    song.addNote (song.addMidiClip (song.firstTrack, 0.0, 16.0), 60, 0.0, 16.0);
    const phazite::ParameterRef lane { synth, "resonance" };
    song.addAutomation (lane, { { phazite::BeatPosition(), 0.1f }, { phazite::BeatPosition::fromBeats (16.0), 0.9f } });
    phazite::setAutomationMode (song.context, song.firstTrack, phazite::AutomationMode::touch);
    const auto handle = phazite::getParameter (song.context, lane);

    phazite::ui::SynthEditor editor (phazite::getDeviceHandle (song.context, synth));
    TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& filter = editor.getFilter();

    song.run ("transport.playStop");
    playback.play (0.5); // to beat 1, where the lane is at 0.15
    const auto grabbed = handle.getValue();
    REQUIRE (grabbed == doctest::Approx (0.15f).epsilon (0.1));

    // Held on its dot, still, for a second: the parameter stays where it was grabbed, as the lane rises under it.
    Mouse mouse (filter);
    mouse.press (filter.getDotPosition().roundToInt());
    playback.play (1.0);
    CHECK (handle.getValue() == doctest::Approx (grabbed).epsilon (0.01));
    mouse.release();
    playback.play (0.5);
    CHECK (handle.getValue() > grabbed + 0.05f); // the lane again
    song.run ("transport.playStop");
}

TEST_CASE ("A drag of one edge of the saturator's band, in Touch while the song plays, grabs that edge alone: the other's lane plays on")
{
    using phazite::devices::Saturator;

    struct Case
    {
        const char* dragged;
        const char* laned;
    };

    // From dragged down (it never reaches To) with a lane on To; To dragged down with a lane on From.
    for (const auto& [dragged, laned] : { Case { "bandLow", "bandHigh" }, Case { "bandHigh", "bandLow" } })
    {
        CAPTURE (dragged);
        TestSong song;
        song.setTempo (120.0);
        phazite::test::EnginePlayback playback;
        song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        song.addNote (song.addMidiClip (song.firstTrack, 0.0, 16.0), 60, 0.0, 16.0);
        const auto saturator = song.addEffect (song.firstTrack, Saturator::typeID);
        song.setParameter (saturator, "band", 1.0f);
        phazite::setAutomationMode (song.context, song.firstTrack, phazite::AutomationMode::touch);

        phazite::ui::SaturatorEditor editor (phazite::getDeviceHandle (song.context, saturator));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto& view = editor.getBandView();
        auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();
        const auto y = view.getHeight() / 2;

        const phazite::ParameterRef lane { saturator, laned };
        const auto rising = std::string (laned) == "bandHigh" ? std::pair { 8000.0f, 12000.0f } : std::pair { 300.0f, 600.0f }; // in hertz
        song.addAutomation (lane,
                            { { phazite::BeatPosition(), rising.first }, { phazite::BeatPosition::fromBeats (16.0), rising.second } });
        const auto pointsBefore = phazite::getAutomationPoints (song.context, lane);
        const auto laneHandle = phazite::getParameter (song.context, lane);

        song.run ("transport.playStop");
        playback.play (0.5);
        ticker.tickNow();
        const auto atPress = laneHandle.getValue();
        const juce::Point<int> edge (juce::roundToInt (editor.getEdgeX (std::string (dragged) == "bandHigh")) - 1, y);
        Mouse mouse (view);
        mouse.press (edge);
        mouse.dragTo (edge - juce::Point<int> (10, 0));
        mouse.dragTo (edge - juce::Point<int> (20, 0));
        playback.play (1.0);
        CHECK (laneHandle.getValue() > atPress); // its lane plays on under the drag
        mouse.release();
        mouse.pause();
        playback.play (0.5);
        song.run ("transport.playStop");
        playback.play (0.1);

        // Its lane as it was: no pass written over it.
        const auto pointsAfter = phazite::getAutomationPoints (song.context, lane);
        REQUIRE (pointsAfter.size() == pointsBefore.size());

        for (size_t i = 0; i < pointsAfter.size(); ++i)
        {
            CHECK (pointsAfter[i].beat.inBeats() == doctest::Approx (pointsBefore[i].beat.inBeats()));
            CHECK (pointsAfter[i].position == doctest::Approx (pointsBefore[i].position));
        }
    }
}

TEST_CASE (
    "A click on a device's graph leaves the keys where they were: only the dynamics curve is a Tab stop, and the keys do what they did")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    phazite::ui::WindowKeys keys (screen.window, screen.shortcutKeys, screen.keyboard);
    Invoked invoked;
    screen.registry.addListener (&invoked);
    const auto devices = screen.addEveryOwnDevice();
    const juce::StringArray graphTitles { "EQ curve",       "Reverb decay",  "Filter response", "Echoes",          "Record", "Bands",
                                          "Saturation",     "Vocoder bands", "Stereo image",    "Osc 1 Wavetable", "Sound",  "Excited band",
                                          "Phaser notches", "Flanger comb",  "Chorus copies",   "Breakpoints" };

    // Every device's graphs, in the dock: none wants the keyboard's focus, but the dynamics curve, whose points the keys
    // choose and move (step S6.3), which Tab reaches and a click on it doesn't take the keys to.
    for (const auto device : devices)
    {
        auto& view = screen.showInDevicesView (device);
        REQUIRE (view.getShownBody() != nullptr);

        for (auto* each : phazite::test::findAll<juce::Component> (*view.getShownBody()))
        {
            const auto isControl = dynamic_cast<juce::Button*> (each) != nullptr || dynamic_cast<juce::Slider*> (each) != nullptr
                                   || dynamic_cast<juce::ComboBox*> (each) != nullptr || dynamic_cast<juce::TextEditor*> (each) != nullptr;
            const auto isGraph =
                dynamic_cast<phazite::ui::DynamicsCurve*> (each) != nullptr
                || dynamic_cast<phazite::ui::LevelHistoryView*> (each) != nullptr
                || dynamic_cast<phazite::ui::EnvelopeView*> (each) != nullptr || dynamic_cast<phazite::ui::LfoView*> (each) != nullptr
                || dynamic_cast<phazite::ui::FilterView*> (each) != nullptr || (! isControl && graphTitles.contains (each->getTitle()));
            CAPTURE (each->getTitle());

            if (dynamic_cast<phazite::ui::DynamicsCurve*> (each) != nullptr)
                CHECK_FALSE (each->getMouseClickGrabsKeyboardFocus());
            else if (isGraph)
                CHECK_FALSE (each->getWantsKeyboardFocus());
        }
    }

    // A device of a type, as addEveryOwnDevice() added them, in the device list's order.
    const auto types = phazite::getDeviceTypes (screen.session.getContext());
    const auto findDevice = [&types, &devices] (const char* typeID)
    {
        size_t at = 0;

        for (const auto& type : types)
        {
            if (! type.isBuiltIn)
                continue;

            if (type.description.fileOrIdentifier == juce::String (typeID))
                return devices[at];

            ++at;
        }

        return phazite::DeviceID();
    };

    // The EQ shown, one of its knobs given the keys, then a click on its graph.
    screen.showInDevicesView (findDevice (phazite::devices::Equalizer::typeID));
    auto* eq = phazite::test::findAll<phazite::ui::EqEditor> (screen.shell).getFirst();
    REQUIRE (eq != nullptr);
    REQUIRE (eq->isShowing());
    // The Output knob's: the band's knobs are linked again to the band a click selects, which takes their keys away.
    phazite::ui::Knob* knob = nullptr;

    for (auto* each : phazite::test::findAll<phazite::ui::Knob> (*eq))
        if (each->getTitle() == "Output")
            knob = each;

    REQUIRE (knob != nullptr);
    knob->grabKeyboardFocus();
    REQUIRE (juce::Component::getCurrentlyFocusedComponent() == knob);
    // The graph takes none, and gives them to no control beside it: the knob keeps them, or the window has them (a press away
    // from a focused slider lets it go: ui/input/TextFieldRelease.h).
    Mouse mouse (eq->getGraph());
    mouse.click (eq->getDotPosition (3).roundToInt());
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    CHECK ((focused == nullptr || focused == knob));

    // Escape, with no drag, goes on to the window's commands (the Smart Tool); Space plays and stops.
    invoked.ids.clear();
    screen.window.pressKey (escape);
    CHECK (invoked.ids.contains ("tool.smart"));
    invoked.ids.clear();
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));
    CHECK (invoked.ids.contains ("transport.playStop"));
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));

    // The computer keyboard takes its letters as before: O plays a note, and runs no shortcut.
    screen.keyboard.setEnabled (true);
    invoked.ids.clear();
    screen.window.pressKey (juce::KeyPress ('o'));
    CHECK (invoked.ids.isEmpty());
    screen.keyboard.releaseAll();
    screen.keyboard.setEnabled (false);

    // The drum sampler's pads: their map keeps the keys a click on the sound's drawing leaves it, and moves a row with Up.
    screen.showInDevicesView (findDevice (phazite::devices::DrumSampler::typeID));
    auto* drums = phazite::test::findAll<phazite::ui::DrumSamplerEditor> (screen.shell).getFirst();
    REQUIRE (drums != nullptr);
    REQUIRE (drums->getPadMap().isShowing());
    REQUIRE (drums->getWaveform().isShowing());
    Mouse padMouse (drums->getPadMap());
    padMouse.click (drums->getPadMap().getLocalBounds().getCentre());
    REQUIRE (juce::Component::getCurrentlyFocusedComponent() == &drums->getPadMap());
    Mouse waveMouse (drums->getWaveform());
    waveMouse.click (drums->getWaveform().getLocalBounds().getCentre());
    CHECK (juce::Component::getCurrentlyFocusedComponent() == &drums->getPadMap());
    const auto firstShown = drums->getFirstShownPad();
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::upKey));
    CHECK (drums->getFirstShownPad() != firstShown);

    screen.registry.removeListener (&invoked);
}

TEST_CASE ("The compressor's ducking threshold, the sampler's loop band and each motion effect's dot take their handles as every "
           "graph does")
{
    TestSong song;

    SUBCASE ("The compressor's threshold line while it ducks")
    {
        using phazite::devices::Compressor;
        const auto compressor = song.addEffect (song.firstTrack, Compressor::typeID);
        phazite::getParameter (song.context, { compressor, "mode" }).setValue ((float) Compressor::Mode::duck);
        phazite::ui::CompressorEditor editor (phazite::getDeviceHandle (song.context, compressor));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        editor.refresh();
        auto& history = editor.getHistory();
        REQUIRE (history.hasLine());
        REQUIRE (history.isShowing());
        const auto threshold = valueOf (song, compressor, "threshold");
        const auto line = juce::Point<float> ((float) history.getWidth() / 2.0f, history.getYForLevel (threshold)).roundToInt();
        pressAndLetGo (song, history, line + juce::Point<int> (0, 2));
        dragThenEscape (song, window, history, line, line + juce::Point<int> (0, 30));
        dragBy (history, line, { 0, 20 });
        CHECK (valueOf (song, compressor, "threshold") < threshold);
    }

    SUBCASE ("The sampler's loop band")
    {
        using phazite::devices::Sampler;
        const auto sampler = song.setInstrument (song.firstTrack, Sampler::typeID);

        for (const auto& [id, value] : { std::pair { "loopOn", 1.0f }, std::pair { "loopStart", 0.25f }, std::pair { "loopEnd", 0.5f } })
            phazite::getParameter (song.context, { sampler, id }).setValue (value);

        phazite::ui::SamplerEditor editor (phazite::getDeviceHandle (song.context, sampler));
        TestWindow window (editor, editor.getWidth(), editor.getHeight());
        editor.refresh();
        auto& view = editor.getSampleView();
        const auto inBand =
            juce::Point<float> ((editor.getMarkerX (Sampler::loopStart) + editor.getMarkerX (Sampler::loopEnd)) / 2.0f, 12.0f).roundToInt();
        pressAndLetGo (song, view, inBand);
        dragThenEscape (song, window, view, inBand, inBand + juce::Point<int> (view.getWidth() / 8, 0));
        dragBy (view, inBand, { view.getWidth() / 8, 0 });
        CHECK (valueOf (song, sampler, "loopStart") > 0.3f);
        CHECK (valueOf (song, sampler, "loopEnd") - valueOf (song, sampler, "loopStart") == doctest::Approx (0.25f).epsilon (0.01));
    }

    SUBCASE ("The dots of Motion's chorus, flanger and phaser (GraphDot)")
    {
        using phazite::devices::Motion;

        struct Case
        {
            const char* typeID = nullptr;
            std::function<juce::Component&(juce::Component&)> graphOf {};
            std::function<juce::Point<float> (juce::Component&)> dotOf {};
            int mode = -1; // Motion's, set before its editor is made
        };

        const auto motionPicture = [] (juce::Component& e) -> juce::Component&
        { return dynamic_cast<phazite::ui::MotionEditor&> (e).getPicture(); };
        const auto motionDot = [] (juce::Component& e) { return dynamic_cast<phazite::ui::MotionEditor&> (e).getDotPosition(); };

        const Case cases[] = {
            { Motion::typeID, motionPicture, motionDot, (int) Motion::Mode::chorus },
            { Motion::typeID, motionPicture, motionDot, (int) Motion::Mode::flanger },
            { Motion::typeID, motionPicture, motionDot, (int) Motion::Mode::phaser },
        };

        for (const auto& c : cases)
        {
            CAPTURE (c.typeID);
            CAPTURE (c.mode);
            const auto device = song.addEffect (song.firstTrack, c.typeID);

            if (c.mode >= 0)
                phazite::setValueSetByHand (songOf (song), phazite::ParameterRef { device, "mode" }, (float) c.mode);

            EditorScreen screen (song, device);
            auto& graph = c.graphOf (*screen.editor);
            REQUIRE (graph.isShowing());
            const auto dot = c.dotOf (*screen.editor);
            const auto edge = (dot + juce::Point<float> (5.0f, 0.0f)).roundToInt();
            pressAndLetGo (song, graph, edge);
            CHECK (c.dotOf (*screen.editor).getDistanceFrom (dot) < 0.01f);
            dragThenEscape (song, *screen.window, graph, edge, edge + juce::Point<int> (-30, 15));

            // From its edge, 20 left: the dot moves as far, and no higher or lower.
            dragBy (graph, edge, { -20, 0 });
            CHECK (c.dotOf (*screen.editor).getDistanceFrom (dot + juce::Point<float> (-20.0f, 0.0f)) < 0.5f);
        }
    }
}

TEST_CASE ("An EQ played into shows its band's modulation, and a drag on its dot moves the gain set by hand by the drag alone (known bug "
           "117)")
{
    // With nothing played into it the EQ sleeps, and what its parameters play then reads as the values set by hand: the
    // modulation doesn't show (described as a known bug in the review's report). A synth plays into it here.
    TestSong song;
    phazite::test::EnginePlayback playback;
    song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
    modulateAndPlay (song, playback, { eq, "band4Gain" }, 0.5f, true);
    const auto handle = phazite::getParameter (song.context, { eq, "band4Gain" });
    REQUIRE (std::abs (handle.getValue() - handle.getBaseValue()) > 3.0f);

    phazite::ui::EqEditor editor (phazite::getDeviceHandle (song.context, eq));
    TestWindow window (editor, editor.getWidth(), editor.getHeight());
    editor.refresh(); // the dot where the modulation plays it
    auto& graph = editor.getGraph();
    dragBy (graph, editor.getDotPosition (3).roundToInt(), { 0, -20 });
    CHECK (valueOf (song, eq, "band4Gain") == doctest::Approx (20.0 / graph.getHeight() * 48.0).epsilon (0.002));
    CHECK (valueOf (song, eq, "band4Frequency") == 800.0f);
}
