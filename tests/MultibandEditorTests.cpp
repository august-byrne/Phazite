// SPDX-License-Identifier: AGPL-3.0-or-later
// The multiband's editor (src/ui/devices/MultibandEditor.h): a control for every parameter but its curves' points, the
// selected band's showing; all of it on one page in the default dock, the bands' graph always with its knobs; a band's level
// dragged in the graph and turned on its knob, the same parameter; the pointer and the hint over everything a drag moves; the
// curve (src/ui/devices/DynamicsCurve.h, the compressor's too), its line and its area lit up to the level heard, its points,
// tension handles and menus, each one undo step; and the Sidechain part, its key's track named in Monitor.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Multiband.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/MultibandEditor.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/theme/DesignTokens.h"

namespace
{
using phazite::devices::Multiband;
namespace curves = phazite::devices::curves;

// The default dock, beside the Devices view's column.
constexpr int dockWidth = 552, dockHeight = 268;

// A multiband on a track of its own song, open in a context of its own whose session plays it.
struct Song
{
    Song()
    {
        phazite::setSong (context, song.get());
        multiband =
            phazite::insertDeviceState (*song, phazite::getTrackIDs (*song)[0], phazite::makeOwnDeviceState (*song, Multiband::typeID));
        REQUIRE (multiband.isValid());
        phazite::getSession (context)->update();
        song->getUndoManager().clearUndoHistory();
    }

    ~Song() { phazite::setSong (context, nullptr); }

    phazite::DeviceHandle getDevice() const { return phazite::makeDeviceHandle (*song, multiband); }

    float valueOf (std::string_view id) const
    {
        return phazite::getValueSetByHand (*song, phazite::ParameterRef { multiband, juce::String (id.data(), id.size()) });
    }

    float pointValue (int band, int point, curves::PointField field) const
    {
        return valueOf (Multiband::info.parameters[(size_t) Multiband::getPointParameter (band, point, field)].id);
    }

    float pointCount (int band) const
    {
        return valueOf (Multiband::info.parameters[(size_t) Multiband::getBandParameter (band, Multiband::points)].id);
    }

    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::DeviceID multiband;
};

bool isPoint (std::string_view id)
{
    return id.find ("Point") != std::string_view::npos && id.find ("Points") == std::string_view::npos;
}

// Whether a parameter's control shows with a band selected: a global one always (but the key's cuts only where the Sidechain
// part shows), and the low, mid and high bands' levels, under the bands' graph; a band's solo with that band, Mid Drives
// Master with the master, and a band's own with it.
bool showsWith (int index, int band, bool sidechainShows)
{
    if (index == Multiband::keyLowCut || index == Multiband::keyHighCut)
        return sidechainShows;

    for (int other = Multiband::low; other <= Multiband::high; ++other)
    {
        if (index == Multiband::getBandParameter (other, Multiband::post))
            return true;

        if (index == Multiband::getSoloParameter (other))
            return band == other;
    }

    if (index == Multiband::midDrivesMaster)
        return band == Multiband::master;

    if (index < Multiband::numGlobals)
        return true;

    return index >= Multiband::getBandParameter (band, Multiband::mode) && index < Multiband::getBandParameter (band + 1, Multiband::mode);
}

// Every control that shows with a band selected shows whole inside the editor: none squeezed out of its room, none hidden.
void checkShownWhole (phazite::ui::MultibandEditor& editor, int band, bool sidechainShows)
{
    for (int i = 0; i < (int) Multiband::info.parameters.size(); ++i)
    {
        const auto& parameter = Multiband::info.parameters[(size_t) i];
        auto* control = editor.findControl (parameter.id);

        if (control == nullptr)
            continue;

        CAPTURE (std::string (parameter.id));
        const auto shown = showsWith (i, band, sidechainShows);
        CHECK (control->isShowing() == shown);

        if (shown)
        {
            CHECK (control->getWidth() > 0);
            CHECK (control->getHeight() > 0);
            CHECK (editor.getLocalBounds().contains (editor.getLocalArea (control, control->getLocalBounds())));
        }
    }
}

juce::String getTooltipOf (juce::Component& component)
{
    auto* client = dynamic_cast<juce::TooltipClient*> (&component);
    REQUIRE (client != nullptr);
    return client->getTooltip();
}
} // namespace

TEST_CASE ("The multiband's editor has a control for every parameter but its curves' points, the selected band's showing")
{
    Song song;
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::MultibandEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);
    CHECK (dynamic_cast<phazite::ui::MultibandEditor*> (phazite::ui::createDeviceEditor (song.context, song.multiband).get()) != nullptr);

    // In a window of its own, as wide as it wants: Monitor and the Sidechain part beside the band, without their switch and tab.
    phazite::ui::MultibandEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    CHECK (editor.getSelectedBand() == Multiband::master); // as Maximus opens

    for (const auto& parameter : Multiband::info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        // The curves' points are the curve's to edit.
        const auto id = juce::String (parameter.id.data(), parameter.id.size());
        const auto hasNone = isPoint (parameter.id) || id.endsWith ("Points");
        CHECK ((editor.findControl (parameter.id) != nullptr) == ! hasNone);
    }

    CHECK (editor.isMonitorShowing());
    CHECK (editor.isSidechainShowing());
    CHECK_FALSE (editor.getMonitorButton().isVisible());
    CHECK_FALSE (editor.getSidechainTab().isVisible());

    // The master's controls show, then the mid band's, picked by its tab; the master has no solo, and Mid Drives is its.
    checkShownWhole (editor, Multiband::master, true);
    CHECK (editor.findControl ("masterPre")->isShowing());
    CHECK (editor.findControl ("masterPost")->isShowing());
    CHECK (editor.findControl ("midDrivesMaster")->isShowing());
    CHECK_FALSE (editor.findControl ("midPre")->isShowing());
    phazite::test::Mouse mouse (editor.getBandButton (Multiband::mid));
    mouse.click (editor.getBandButton (Multiband::mid).getLocalBounds().getCentre());
    CHECK (editor.getSelectedBand() == Multiband::mid);
    CHECK (editor.findControl ("midPre")->isShowing());
    CHECK (editor.findControl ("midSolo")->isShowing());
    CHECK (editor.findControl ("midKey")->isShowing());
    CHECK_FALSE (editor.findControl ("masterPre")->isShowing());
    CHECK (editor.findControl ("lmhMix")->isShowing());  // the bands' knobs always show
    CHECK (editor.findControl ("midPost")->isShowing()); // the mid band's level, under the bands' graph
    checkShownWhole (editor, Multiband::mid, true);
    phazite::test::checkChildrenApart (editor);
    phazite::test::checkChildrenApart (*editor.getLayout().getContent (1, 0));

    // Monitor's traces are in its Traces menu, each ticked while it shows; an item switches it.
    const auto menu = editor.getTracesMenu();

    for (const auto* name : { "In", "Band In", "Out", "Heard", "Gain", "All Gains", "Pre/Post" })
        CHECK (phazite::test::findMenuItem (menu, { name }) != nullptr);

    REQUIRE (phazite::test::findMenuItem (menu, { "Heard" }) != nullptr);
    const auto heard = phazite::test::findMenuItem (menu, { "Heard" })->isTicked;
    REQUIRE (phazite::test::runMenuItem (menu, { "Heard" }));
    const auto after = editor.getTracesMenu();
    CHECK (phazite::test::findMenuItem (after, { "Heard" })->isTicked != heard);
}

TEST_CASE ("The multiband's editor fits the default dock on one page, its bands' graph with every one of its knobs")
{
    // The owner, 2026-11-10: "the multiband compressor can't be separating the knobs for the frequency bands/graph from the
    // graph by putting it on a separate tab. you must figure out how to put everything on one page."
    Song song;
    phazite::ui::MultibandEditor editor (song.getDevice());
    REQUIRE (editor.getMinimumHeightAtWidth (dockWidth) <= dockHeight);
    phazite::test::TestWindow window (editor, dockWidth, dockHeight);
    CHECK_FALSE (editor.getLayout().usesPages());
    CHECK_FALSE (editor.getLayout().usesTabs (0));
    CHECK_FALSE (editor.getLayout().usesTabs (1));

    // The graph, and the knobs it is drawn by beside it: the crossovers with their slopes, the low cut, the bands' levels and
    // LMH Mix and Delay, every one whole in the editor.
    CHECK (editor.getBandsGraph().isShowing());
    CHECK (editor.getBandsGraph().getWidth() >= 200);

    for (const auto* id :
         { "lowCut", "lowFrequency", "highFrequency", "lowSlope", "highSlope", "lowPost", "midPost", "highPost", "lmhMix", "lmhDelay" })
    {
        CAPTURE (id);
        auto* control = editor.findControl (id);
        REQUIRE (control != nullptr);
        CHECK (control->isShowing());
        CHECK (editor.getLocalBounds().contains (editor.getLocalArea (control, control->getLocalBounds())));
    }

    // And the band beside them: its curve, and all of its controls but the key's cuts, which are the Sidechain part's.
    CHECK (editor.getCurve().isShowing());
    CHECK (editor.getCurve().getWidth() >= 116);
    checkShownWhole (editor, Multiband::master, false);
    phazite::test::checkChildrenApart (editor);

    for (int column = 0; column < 2; ++column)
        phazite::test::checkChildrenApart (*editor.getLayout().getContent (column, 0));

    phazite::test::checkChildrenApart (editor.getBandsGraph());

    // The Sidechain part by its tab, in the band's place: the key's cuts, Listen; the bands' graph still beside it.
    CHECK_FALSE (editor.isSidechainShowing());
    REQUIRE (editor.getSidechainTab().isShowing());
    phazite::test::Mouse mouse (editor);
    const auto clickOn = [&editor, &mouse] (juce::Component& c)
    { mouse.click (editor.getLocalPoint (&c, c.getLocalBounds().getCentre())); };
    clickOn (editor.getSidechainTab());
    CHECK (editor.isSidechainShowing());
    CHECK (editor.findControl ("keyLowCut")->isShowing());
    CHECK (editor.getListenButton().isShowing());
    CHECK_FALSE (editor.getCurve().isShowing());
    CHECK (editor.getBandsGraph().isShowing());
    phazite::test::checkChildrenApart (*editor.getLayout().getContent (1, 0));

    // A band's tab brings the band back.
    clickOn (editor.getBandButton (Multiband::low));
    CHECK (editor.getSelectedBand() == Multiband::low);
    CHECK_FALSE (editor.isSidechainShowing());
    CHECK (editor.getCurve().isShowing());
    checkShownWhole (editor, Multiband::low, false);

    // Monitor by its switch, in the place of the band's knobs, the curve still showing; and back.
    CHECK_FALSE (editor.isMonitorShowing());
    REQUIRE (editor.getMonitorButton().isShowing());
    clickOn (editor.getMonitorButton());
    CHECK (editor.isMonitorShowing());
    CHECK_FALSE (editor.findControl ("lowPre")->isShowing());
    CHECK (editor.getCurve().isShowing());
    CHECK (editor.findControl ("lowFrequency")->isShowing());
    clickOn (editor.getMonitorButton());
    CHECK_FALSE (editor.isMonitorShowing());
    CHECK (editor.findControl ("lowPre")->isShowing());
}

TEST_CASE ("The multiband's graph: a crossover drags left or right, a band's level up or down, its knob the same, one undo step each")
{
    Song song;
    phazite::ui::MultibandEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, dockWidth, dockHeight);
    auto& graph = editor.getBandsGraph();
    phazite::test::Mouse mouse (graph);
    const auto middle = juce::roundToInt (editor.getBandsY (-12.0f));

    // The low crossover, from 120 Hz to 240 Hz.
    mouse.press ({ juce::roundToInt (editor.getBandsX (120.0)), middle });
    mouse.dragTo ({ juce::roundToInt (editor.getBandsX (180.0)), middle });
    mouse.dragTo ({ juce::roundToInt (editor.getBandsX (240.0)), middle });
    mouse.release();
    CHECK (song.valueOf ("lowFrequency") == doctest::Approx (240.0f).epsilon (0.04));
    CHECK (song.describeUndo() == juce::String ("Undo Change Low Frequency"));

    // The mid band, up by 6 dB: its level, which is its Post (Pre stays as it was), as one undo step.
    const auto inMid = juce::roundToInt (editor.getBandsX (1000.0));
    mouse.press ({ inMid, juce::roundToInt (editor.getBandsY (0.0f)) });
    mouse.dragTo ({ inMid, juce::roundToInt (editor.getBandsY (3.0f)) });
    mouse.dragTo ({ inMid, juce::roundToInt (editor.getBandsY (6.0f)) });
    mouse.release();
    CHECK (std::abs (song.valueOf ("midPost") - 6.0f) < 0.6f); // within a pixel of the graph
    CHECK (song.valueOf ("midPre") == doctest::Approx (0.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Mid Post"));
    phazite::undo (*song.song);
    CHECK (song.valueOf ("midPost") == doctest::Approx (0.0f));
    editor.refresh();

    // Its knob, under the graph, is the same parameter: turned up, one undo step of the same name.
    auto* knob = dynamic_cast<juce::Slider*> (editor.findControl ("midPost"));
    REQUIRE (knob != nullptr);
    CHECK (knob->isShowing());
    phazite::test::Mouse knobMouse (*knob);
    const auto centre = knob->getLocalBounds().getCentre();
    knobMouse.press (centre);
    knobMouse.dragTo (centre - juce::Point<int> (0, 10));
    knobMouse.dragTo (centre - juce::Point<int> (0, 20));
    knobMouse.release();
    const auto turned = song.valueOf ("midPost");
    CHECK (turned > 0.5f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Mid Post"));
    phazite::undo (*song.song);
    CHECK (song.valueOf ("midPost") == doctest::Approx (0.0f));

    // A level set on the knob is where the graph's band stands: dragged from there, it goes on from it.
    song.getDevice().getParameter ("midPost").setValue (-6.0f);
    editor.refresh();
    mouse.press ({ inMid, juce::roundToInt (editor.getBandsY (-6.0f)) });
    mouse.dragTo ({ inMid, juce::roundToInt (editor.getBandsY (-3.0f)) });
    mouse.dragTo ({ inMid, juce::roundToInt (editor.getBandsY (0.0f)) });
    mouse.release();
    CHECK (std::abs (song.valueOf ("midPost")) < 0.6f);

    // A click on a band, without a drag, shows its curve and controls, and changes nothing.
    phazite::undo (*song.song);
    mouse.click ({ juce::roundToInt (editor.getBandsX (60.0)), middle });
    CHECK (editor.getSelectedBand() == Multiband::low);
    CHECK (song.valueOf ("lowPost") == doctest::Approx (0.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Mid Post"));

    // A double-click puts a crossover back where it started.
    mouse.doubleClick ({ juce::roundToInt (editor.getBandsX (240.0)), middle });
    CHECK (song.valueOf ("lowFrequency") == doctest::Approx (120.0f));
}

TEST_CASE ("Over the multiband's graphs, the pointer shows which way a drag goes, what it moves lights up, and its hint says what")
{
    Song song;
    phazite::ui::MultibandEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, dockWidth, dockHeight);
    auto& graph = editor.getBandsGraph();
    phazite::test::Mouse mouse (graph);
    const auto middle = juce::roundToInt (editor.getBandsY (-12.0f));

    // A crossover: sideways; a band's level: up and down; each named in the hint.
    mouse.moveTo ({ juce::roundToInt (editor.getBandsX (5000.0)), middle });
    CHECK (graph.getMouseCursor() == juce::MouseCursor::LeftRightResizeCursor);
    CHECK (getTooltipOf (graph).contains ("crossover between the mid and high bands"));

    mouse.moveTo ({ juce::roundToInt (editor.getBandsX (1000.0)), middle });
    CHECK (graph.getMouseCursor() == juce::MouseCursor::UpDownResizeCursor);
    CHECK (getTooltipOf (graph).contains ("mid band's level"));

    // The low cut's edge, at the left: sideways.
    mouse.moveTo ({ juce::roundToInt (editor.getBandsX (20.0)) + 2, middle });
    CHECK (graph.getMouseCursor() == juce::MouseCursor::LeftRightResizeCursor);
    CHECK (getTooltipOf (graph).contains ("low cut"));

    // Off the graph, the plain pointer, and no hint of its own.
    mouse.moveTo ({ -20, -20 });
    CHECK (graph.getMouseCursor() == juce::MouseCursor::NormalCursor);
    CHECK (getTooltipOf (graph).isEmpty());

    // The curve: a point both ways, the curve's ends and a line's handle up and down, each with its hint.
    auto& curve = editor.getCurve();
    phazite::test::Mouse onCurve (curve);
    onCurve.moveTo (curve.getPointPosition (1).roundToInt());
    CHECK (curve.getMouseCursor() == juce::MouseCursor::DraggingHandCursor);
    CHECK (getTooltipOf (curve).startsWith ("Point 2"));

    onCurve.moveTo (curve.getPointPosition (0).roundToInt());
    CHECK (curve.getMouseCursor() == juce::MouseCursor::UpDownResizeCursor);
    CHECK (getTooltipOf (curve).contains ("start"));

    REQUIRE (curve.getTensionPosition (1).has_value());
    onCurve.moveTo (curve.getTensionPosition (1)->roundToInt());
    CHECK (curve.getMouseCursor() == juce::MouseCursor::UpDownResizeCursor);
    CHECK (getTooltipOf (curve).contains ("bend"));

    // Out, and back in away from any handle: the plain pointer, none left from before (2026-11-10).
    onCurve.moveTo (curve.getPointPosition (1).roundToInt());
    onCurve.moveTo ({ -20, -20 });
    CHECK (curve.getMouseCursor() == juce::MouseCursor::NormalCursor);
    onCurve.moveTo (curve.getPosition (0.9f, 0.1f).roundToInt());
    CHECK (curve.getMouseCursor() == juce::MouseCursor::NormalCursor);
    CHECK (getTooltipOf (curve).isEmpty());
}

TEST_CASE ("The multiband's curve: points drag, lines bend, points are added and deleted, each one undo step")
{
    Song song;
    phazite::ui::MultibandEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    editor.selectBand (Multiband::low);
    auto& curve = editor.getCurve();
    phazite::test::Mouse mouse (curve);

    // The corner at 0 dB, dragged down (Shift keeps it where it is up and down, so here Ctrl keeps it across).
    const auto corner = curve.getPointPosition (1).roundToInt();
    mouse.press (corner, juce::ModifierKeys::ctrlModifier);
    mouse.dragTo (corner + juce::Point<int> (25, 20));
    mouse.release();
    CHECK (song.pointValue (Multiband::low, 1, curves::pointY) < 0.45f);
    CHECK (song.pointValue (Multiband::low, 1, curves::pointX) == doctest::Approx (0.5f)); // Ctrl: across, it stayed
    CHECK (song.describeUndo() == juce::String ("Undo Move Point"));
    phazite::undo (*song.song);
    editor.refresh();
    CHECK (song.pointValue (Multiband::low, 1, curves::pointY) == doctest::Approx (0.5f));

    // The handle of the line before it, dragged up: bent up; right-clicked: straight again.
    REQUIRE (curve.getTensionPosition (1).has_value());
    const auto handle = curve.getTensionPosition (1)->roundToInt();
    mouse.press (handle);
    mouse.dragTo (handle - juce::Point<int> (0, 30));
    mouse.release();
    CHECK (song.pointValue (Multiband::low, 1, curves::pointTension) > 0.1f);
    CHECK (song.describeUndo() == juce::String ("Undo Bend Line"));
    editor.refresh();
    mouse.click (curve.getTensionPosition (1)->roundToInt(), juce::ModifierKeys::rightButtonModifier);
    CHECK (song.pointValue (Multiband::low, 1, curves::pointTension) == doctest::Approx (0.0f));

    // A right-click away from the points adds one there; Alt and a click deletes it.
    const auto empty = curve.getPosition (0.75f, 0.9f).roundToInt();
    mouse.click (empty, juce::ModifierKeys::rightButtonModifier);
    CHECK (song.pointCount (Multiband::low) == doctest::Approx (4.0f));
    CHECK (song.pointValue (Multiband::low, 2, curves::pointX) == doctest::Approx (0.75f).epsilon (0.02));
    CHECK (song.describeUndo() == juce::String ("Undo Add Point"));
    editor.refresh();
    mouse.click (curve.getPointPosition (2).roundToInt(), juce::ModifierKeys::altModifier);
    CHECK (song.pointCount (Multiband::low) == doctest::Approx (3.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Delete Point"));

    // The point menu's shapes, and the curve menu's shapes to start from.
    curve.setPointType (1, (int) curves::PointType::stairs);
    CHECK (song.pointValue (Multiband::low, 1, curves::pointType) == doctest::Approx ((float) curves::PointType::stairs));
    REQUIRE (curve.applyShape ("No Compression"));
    CHECK (song.pointCount (Multiband::low) == doctest::Approx (2.0f));
    CHECK (song.pointValue (Multiband::low, 1, curves::pointY) == doctest::Approx (1.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Curve"));
    CHECK_FALSE (curve.applyShape ("No Such Shape"));
}

TEST_CASE ("In the dock, the multiband's curve's menu holds Freeze and Snap, its toolbar too narrow for their buttons")
{
    Song song;
    phazite::ui::MultibandEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, dockWidth, dockHeight);
    auto* tools = phazite::test::findAll<phazite::ui::DynamicsCurveTools> (editor).getFirst();
    REQUIRE (tools != nullptr);
    CHECK (tools->isCompact());
    CHECK_FALSE (tools->getFreezeButton().isVisible());

    // Freeze from the menu: the curve takes no edits until Freeze is off again.
    const auto menu = tools->getMenu();
    REQUIRE (phazite::test::findMenuItem (menu, { "Snap" }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Snap" })->isTicked);
    REQUIRE (phazite::test::runMenuItem (menu, { "Freeze" }));
    CHECK (editor.getCurve().isFrozen());
    REQUIRE (phazite::test::runMenuItem (tools->getMenu(), { "Freeze" }));
    CHECK_FALSE (editor.getCurve().isFrozen());

    // In a window of its own, wide enough, its three buttons.
    editor.setSize (editor.getWidthForHeight (0), editor.getHeightForWidth (editor.getWidthForHeight (0)));
    CHECK_FALSE (tools->isCompact());
    CHECK (tools->getFreezeButton().isVisible());
}

TEST_CASE ("The multiband's curve draws its line, and lights the area under it up to the level the band's compressor hears")
{
    Song song;
    const auto device = song.getDevice();
    phazite::ui::MultibandEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    editor.selectBand (Multiband::low);
    auto& curve = editor.getCurve();
    const auto pixelAt = [&] (juce::Image& image, float across, float up)
    {
        const auto at = window.getLocalPoint (&curve, curve.getPosition (across, up)).roundToInt();
        return image.getPixelAt (at.x, at.y);
    };
    const auto band = juce::Colour (phazite::ui::partColours[Multiband::low]);
    const auto quiet = curves::axisForDecibels (-24.0f), loud = curves::axisForDecibels (4.5f); // between grid lines

    // The line, up the diagonal to 0 dB: in the band's colour over the grid's grey line (no line at all was drawn, and the
    // area under it a wedge from the first point to the last: 2026-11-01).
    {
        auto image = window.paintEverything();
        const auto onLine = pixelAt (image, 0.3f, 0.3f);
        CHECK (std::abs (onLine.getRed() - band.getRed()) < 40);
        CHECK (std::abs (onLine.getGreen() - band.getGreen()) < 40);

        // In silence, nothing lit.
        CHECK (pixelAt (image, quiet, 0.02f).getRed() < pixelAt (image, loud, 0.02f).getRed() + 10);
    }

    // A tone in the low band, at -6 dB: lit up to it, not past it.
    auto* multiband = device.getProcessor<Multiband>();
    REQUIRE (multiband != nullptr);
    multiband->prepare (48000.0, 480);
    std::vector<float> values;

    for (const auto& parameter : Multiband::info.parameters)
        values.push_back (song.valueOf (parameter.id));

    juce::AudioBuffer<float> buffer (2, 480);

    for (int block = 0, sample = 0; block < 20; ++block)
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i, ++sample)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 60.0f * (float) sample / 48000.0f));

        phazite::devices::ProcessContext context;
        context.channels = buffer.getArrayOfWritePointers();
        context.numChannels = 2;
        context.numSamples = buffer.getNumSamples();
        context.parameters = values;
        multiband->process (context);
    }

    editor.refresh();
    auto image = window.paintEverything();
    CHECK (pixelAt (image, quiet, 0.02f).getRed() > pixelAt (image, loud, 0.02f).getRed() + 40);
}

TEST_CASE ("The multiband's Sidechain part picks the key's track, which Monitor names while the band shown hears it")
{
    Song song;
    const auto kick = phazite::ensureTracks (*song.song, 2)[1];
    phazite::setTrackName (*song.song, kick, "Kick");
    auto shown = phazite::ui::createDeviceEditor (song.context, song.multiband);
    auto* editor = dynamic_cast<phazite::ui::MultibandEditor*> (shown.get());
    REQUIRE (editor != nullptr);
    phazite::test::TestWindow window (*editor, editor->getWidth(), editor->getHeight());

    // Its picker, at the Sidechain part's top; the kick chosen there (as its menu chooses it), the bands whose Key is on hear it.
    auto* picker = editor->getSidechainButton();
    REQUIRE (picker != nullptr);
    CHECK (picker->isShowing());
    CHECK (picker->getY() <= editor->getListenButton().getY());
    phazite::setSidechainSource (song.context, song.multiband, kick);
    editor->refresh();
    CHECK (phazite::getSidechainSource (song.context, song.multiband) == kick);

    // The part says which bands hear the key, under Listen.
    const auto listeners = [editor]
    {
        for (auto* child : editor->getSidechainPart().getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (child))
                return label->getText();

        return juce::String();
    };

    // As it was made, it starts ducking its lows under the kick: Monitor names the kick while the low band shows, which hears
    // it through its Key, and not for another band, nor once the low band's Key is off.
    editor->selectBand (Multiband::low);
    editor->refresh();
    CHECK (song.valueOf ("lowKey") == doctest::Approx (1.0f));
    CHECK (editor->getMonitorKeyName() == juce::String ("Kick"));
    CHECK (listeners() == juce::String ("Heard by the low band, in place of its own sound"));
    song.getDevice().getParameter ("highKey").setValue (1.0f);
    song.getDevice().getParameter ("masterKey").setValue (1.0f);
    editor->refresh();
    CHECK (listeners() == juce::String ("Heard by the low, high and master bands, in place of their own sound"));
    song.getDevice().getParameter ("highKey").setValue (0.0f);
    song.getDevice().getParameter ("masterKey").setValue (0.0f);
    editor->selectBand (Multiband::mid);
    editor->refresh();
    CHECK (editor->getMonitorKeyName().isEmpty());
    song.getDevice().getParameter ("lowKey").setValue (0.0f);
    editor->selectBand (Multiband::low);
    editor->refresh();
    CHECK (editor->getMonitorKeyName().isEmpty());
    CHECK (listeners().startsWith ("No band hears it yet"));

    // Without the project, its editor has no picker.
    phazite::ui::MultibandEditor alone (song.getDevice());
    CHECK (alone.getSidechainButton() == nullptr);
    shown = nullptr;
}
