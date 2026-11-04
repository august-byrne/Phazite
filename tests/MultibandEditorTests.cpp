// SPDX-License-Identifier: AGPL-3.0-or-later
// The multiband's editor (src/ui/devices/MultibandEditor.h), in Maximus's layout: a control for every parameter but its curves'
// points, the selected band's showing; the curve (src/ui/devices/DynamicsCurve.h, the compressor's too), its line and its
// area lit up to the level heard, its points, tension handles and menus, each one undo step; and the Bands view's crossovers
// and band levels.
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
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/MultibandEditor.h"
#include "ui/theme/DesignTokens.h"

namespace
{
using phazite::devices::Multiband;
namespace curves = phazite::devices::curves;

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
    const auto device = song.getDevice();
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

TEST_CASE ("The multiband's Bands view: a band's edge moves its crossover, a band up or down its pre gain")
{
    Song song;
    phazite::ui::MultibandEditor editor ((song.getDevice()));
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
    song.getDevice().getParameter ("highFrequency").setValue (8000.0f);
    editor.refresh();
    CHECK (editor.isShowingBands());

    // When the moment is over, Monitor comes back, repainted though nothing plays.
    window.takeRepaintedArea();
    juce::Thread::sleep (1600);
    editor.refresh();
    CHECK_FALSE (editor.isShowingBands());
    CHECK (window.takeRepaintedArea().intersectsRectangle (window.getLocalArea (&display, display.getLocalBounds())));
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
