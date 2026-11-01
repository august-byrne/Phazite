// SPDX-License-Identifier: AGPL-3.0-or-later
// The multiband's editor (src/ui/devices/MultibandEditor.h), in Maximus's layout: a control for every parameter but its curves'
// points, the selected band's showing; the curve's points, tension handles and menus, each one undo step; and the Bands view's
// crossovers and band levels.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Multiband.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/MultibandEditor.h"

namespace
{
using phazite::devices::Multiband;

// A multiband on a track of its own Edit.
struct Song
{
    Song()
    {
        plugin = edit->getPluginCache().createNewPlugin (Multiband::typeID, {});
        phazite::insertDevice (*te::getAudioTracks (*edit)[0], plugin);
        phazite::test::settle();
        phazite::getSongUndoManager (*edit).clearUndoHistory();
    }

    ~Song() { plugin = nullptr; }

    float valueOf (std::string_view id) const
    {
        return plugin->getAutomatableParameterByID (juce::String (id.data(), id.size()))->getCurrentValue();
    }

    float pointValue (int band, int point, Multiband::PointField field) const
    {
        return valueOf (Multiband::info.parameters[(size_t) Multiband::getPointParameter (band, point, field)].id);
    }

    float pointCount (int band) const
    {
        return valueOf (Multiband::info.parameters[(size_t) Multiband::getBandParameter (band, Multiband::points)].id);
    }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine());
    te::Plugin::Ptr plugin;
};

bool isPoint (std::string_view id)
{
    return id.find ("Point") != std::string_view::npos && id.find ("Points") == std::string_view::npos;
}

// Every control of a band, and every global one, shows whole inside the editor: none squeezed out of its room.
void checkShownWhole (phazite::ui::MultibandEditor& editor, int band)
{
    for (int i = 0; i < (int) Multiband::info.parameters.size(); ++i)
    {
        const auto& parameter = Multiband::info.parameters[(size_t) i];
        const auto ofBand =
            i >= Multiband::getBandParameter (band, Multiband::mode) && i < Multiband::getBandParameter (band + 1, Multiband::mode);

        if (auto* control = editor.findControl (parameter.id); control != nullptr && (i < Multiband::numGlobals || ofBand))
        {
            CAPTURE (std::string (parameter.id));
            CHECK (control->isShowing());
            CHECK (control->getWidth() > 0);
            CHECK (control->getHeight() > 0);
            CHECK (editor.getLocalBounds().contains (editor.getLocalArea (control, control->getLocalBounds())));
        }
    }
}
} // namespace

TEST_CASE ("The multiband's editor has a control for every parameter but its curves' points, the selected band's showing")
{
    Song song;
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::MultibandEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::MultibandEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    CHECK (editor.getSelectedBand() == Multiband::master); // as Maximus opens

    for (const auto& parameter : Multiband::info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        // The curves' points are the curve's to edit; the master has no solo, as in Maximus.
        const auto id = juce::String (parameter.id.data(), parameter.id.size());
        const auto hasNone = isPoint (parameter.id) || id.endsWith ("Points") || id == "masterSolo";
        CHECK ((editor.findControl (parameter.id) != nullptr) == ! hasNone);
    }

    // The master's controls show, then the mid band's, picked by its tab; the master has no solo.
    checkShownWhole (editor, Multiband::master);
    CHECK (editor.findControl ("masterPre")->isShowing());
    CHECK_FALSE (editor.findControl ("midPre")->isShowing());
    phazite::test::Mouse mouse (editor.getBandButton (Multiband::mid));
    mouse.click (editor.getBandButton (Multiband::mid).getLocalBounds().getCentre());
    CHECK (editor.getSelectedBand() == Multiband::mid);
    CHECK (editor.findControl ("midPre")->isShowing());
    CHECK (editor.findControl ("midSolo")->isShowing());
    CHECK_FALSE (editor.findControl ("masterPre")->isShowing());
    CHECK (editor.findControl ("lmhMix")->isShowing()); // the global controls always show
    checkShownWhole (editor, Multiband::mid);
    phazite::test::checkChildrenApart (editor.getDisplay());

    // Monitor's switches for its traces share their room evenly, each wide enough for its name.
    const juce::StringArray traceNames { "In", "Band In", "Out", "Heard", "Gain", "All Gains", "Pre/Post" };
    juce::Array<int> widths;

    for (auto* button : phazite::test::findAll<juce::TextButton> (editor))
        if (traceNames.contains (button->getButtonText()))
            widths.addIfNotAlreadyThere (button->getWidth());

    CHECK (widths.size() == 1);
    CHECK (widths.getFirst() >= 56);
}

TEST_CASE ("The multiband's curve: points drag, lines bend, points are added and deleted, each one undo step")
{
    Song song;
    phazite::ui::MultibandEditor editor ((phazite::makeDeviceHandle (*song.plugin)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    editor.selectBand (Multiband::low);
    auto& curve = editor.getCurve();
    phazite::test::Mouse mouse (curve);

    // The corner at 0 dB, dragged down (Shift keeps it where it is up and down, so here Ctrl keeps it across).
    const auto corner = editor.getPointPosition (1).roundToInt();
    mouse.press (corner, juce::ModifierKeys::ctrlModifier);
    mouse.dragTo (corner + juce::Point<int> (25, 20));
    mouse.release();
    CHECK (song.pointValue (Multiband::low, 1, Multiband::pointY) < 0.45f);
    CHECK (song.pointValue (Multiband::low, 1, Multiband::pointX) == doctest::Approx (0.5f)); // Ctrl: across, it stayed
    CHECK (song.describeUndo() == juce::String ("Undo Move Point"));
    phazite::undo (*song.edit);
    editor.refresh();
    CHECK (song.pointValue (Multiband::low, 1, Multiband::pointY) == doctest::Approx (0.5f));

    // The handle of the line before it, dragged up: bent up; right-clicked: straight again.
    REQUIRE (editor.getTensionPosition (1).has_value());
    const auto handle = editor.getTensionPosition (1)->roundToInt();
    mouse.press (handle);
    mouse.dragTo (handle - juce::Point<int> (0, 30));
    mouse.release();
    CHECK (song.pointValue (Multiband::low, 1, Multiband::pointTension) > 0.1f);
    CHECK (song.describeUndo() == juce::String ("Undo Bend Line"));
    editor.refresh();
    mouse.click (editor.getTensionPosition (1)->roundToInt(), juce::ModifierKeys::rightButtonModifier);
    CHECK (song.pointValue (Multiband::low, 1, Multiband::pointTension) == doctest::Approx (0.0f));

    // A right-click away from the points adds one there; Alt and a click deletes it.
    const auto empty = editor.getCurvePosition (0.75f, 0.9f).roundToInt();
    mouse.click (empty, juce::ModifierKeys::rightButtonModifier);
    CHECK (song.pointCount (Multiband::low) == doctest::Approx (4.0f));
    CHECK (song.pointValue (Multiband::low, 2, Multiband::pointX) == doctest::Approx (0.75f).epsilon (0.02));
    CHECK (song.describeUndo() == juce::String ("Undo Add Point"));
    editor.refresh();
    mouse.click (editor.getPointPosition (2).roundToInt(), juce::ModifierKeys::altModifier);
    CHECK (song.pointCount (Multiband::low) == doctest::Approx (3.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Delete Point"));

    // The point menu's shapes, and the curve menu's shapes to start from.
    editor.setPointType (1, (int) Multiband::PointType::stairs);
    CHECK (song.pointValue (Multiband::low, 1, Multiband::pointType) == doctest::Approx ((float) Multiband::PointType::stairs));
    REQUIRE (editor.applyShape ("No Compression"));
    CHECK (song.pointCount (Multiband::low) == doctest::Approx (2.0f));
    CHECK (song.pointValue (Multiband::low, 1, Multiband::pointY) == doctest::Approx (1.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Curve"));
    CHECK_FALSE (editor.applyShape ("No Such Shape"));
}

TEST_CASE ("The multiband's Bands view: a band's edge moves its crossover, a band up or down its pre gain")
{
    Song song;
    phazite::ui::MultibandEditor editor ((phazite::makeDeviceHandle (*song.plugin)));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    editor.showBands (true);
    CHECK (editor.isShowingBands());
    auto& display = editor.getDisplay();
    phazite::test::Mouse mouse (display);
    const auto middle = juce::roundToInt (editor.getBandsY (-12.0f));

    // The low crossover, from 120 Hz to 240 Hz.
    mouse.press ({ juce::roundToInt (editor.getBandsX (120.0)), middle });
    mouse.dragTo ({ juce::roundToInt (editor.getBandsX (240.0)), middle });
    mouse.release();
    CHECK (song.valueOf ("lowFrequency") == doctest::Approx (240.0f).epsilon (0.03));

    // The mid band, up to +6 dB.
    const auto inMid = juce::roundToInt (editor.getBandsX (1000.0));
    mouse.press ({ inMid, juce::roundToInt (editor.getBandsY (0.0f)) });
    mouse.dragTo ({ inMid, juce::roundToInt (editor.getBandsY (6.0f)) });
    mouse.release();
    CHECK (song.valueOf ("midPre") == doctest::Approx (6.0f).epsilon (0.05));

    // Under Monitor, a crossover moved elsewhere brings Bands up for a moment.
    editor.showBands (false);
    CHECK_FALSE (editor.isShowingBands());
    phazite::makeDeviceHandle (*song.plugin).getParameter ("highFrequency").setValue (8000.0f);
    editor.refresh();
    CHECK (editor.isShowingBands());

    // When the moment is over, Monitor comes back, repainted though nothing plays.
    window.takeRepaintedArea();
    juce::Thread::sleep (1600);
    editor.refresh();
    CHECK_FALSE (editor.isShowingBands());
    CHECK (window.takeRepaintedArea().intersectsRectangle (window.getLocalArea (&display, display.getLocalBounds())));
}
