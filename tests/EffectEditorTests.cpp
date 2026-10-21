// SPDX-License-Identifier: AGPL-3.0-or-later
// The Filter's, the saturator's, the delay's, the stereo expander's, the phaser's, the flanger's and the vocoder's editors
// (src/ui/devices/): a control for each parameter but the mix (the header's), what their pictures show, and the drags on
// them, each one undo step.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/vocoder/Vocoder.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "ui/devices/DelayEditor.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/FilterEditor.h"
#include "ui/devices/FlangerEditor.h"
#include "ui/devices/PhaserEditor.h"
#include "ui/devices/SaturatorEditor.h"
#include "ui/devices/StereoExpanderEditor.h"
#include "ui/devices/VocoderEditor.h"

namespace
{
using phazite::devices::Delay;
using phazite::devices::FilterEffect;
using phazite::devices::Saturator;

// A device on a track of its own Edit.
struct Song
{
    explicit Song (const char* typeID)
    {
        plugin = edit->getPluginCache().createNewPlugin (typeID, {});
        te::getAudioTracks (*edit)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
        phazite::test::settle();
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { plugin = nullptr; }

    float valueOf (const char* id) const { return plugin->getAutomatableParameterByID (id)->getCurrentValue(); }
    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr plugin;
};

void checkEveryParameterButMixHasAControl (const phazite::ui::DeviceEditorBase& editor, const phazite::devices::DeviceInfo& info)
{
    for (const auto& parameter : info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        CHECK ((editor.findControl (parameter.id) != nullptr) == (parameter.id != phazite::devices::mixParameterID));
    }
}
} // namespace

TEST_CASE ("The Filter's editor draws its response, and its dot drags the cutoff and resonance as one undo step")
{
    Song song (FilterEffect::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::FilterEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::FilterEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, FilterEffect::info);
    phazite::test::checkChildrenApart (editor);

    // From fully open, a drag to the middle and up: a lower cutoff and more resonance, one step named after the cutoff.
    auto& graph = editor.getGraph();
    phazite::test::Mouse mouse (graph);
    const auto middle = graph.getLocalBounds().getCentre();
    mouse.press (editor.getDotPosition().roundToInt());
    mouse.dragTo (middle);
    mouse.dragTo (middle - juce::Point<int> (0, graph.getHeight() / 4));
    mouse.release();
    CHECK (song.valueOf ("cutoff") < 2000.0f);
    CHECK (song.valueOf ("cutoff") > 200.0f);
    CHECK (song.valueOf ("resonance") > 0.6f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Cutoff"));
    phazite::undo (*song.edit);
    CHECK (song.valueOf ("cutoff") == doctest::Approx (FilterEffect::info.parameters[FilterEffect::cutoff].defaultValue));
    CHECK (song.valueOf ("resonance") == doctest::Approx (0.1f));

    // The wheel turns the drive.
    mouse.wheel (middle, 0.5f);
    CHECK (song.valueOf ("drive") > 0.5f);
}

TEST_CASE ("The saturator's editor picks its shape with buttons drawn as their curves, and shows how far the sound reaches")
{
    Song song (Saturator::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::SaturatorEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::SaturatorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, Saturator::info);
    phazite::test::checkChildrenApart (editor);

    // Soft is lit; a click on Fold picks it, as one undo step.
    CHECK (editor.getShapeButton (0).getToggleState());
    CHECK (editor.getShapeButton (4).getTitle() == juce::String ("Fold"));
    phazite::test::Mouse mouse (editor.getShapeButton (4));
    mouse.click (editor.getShapeButton (4).getLocalBounds().getCentre());
    CHECK (song.valueOf ("shape") == doctest::Approx (4.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Shape"));
    CHECK (editor.getShapeButton (4).getToggleState());
    CHECK_FALSE (editor.getShapeButton (0).getToggleState());

    // A drag up the curve turns the drive up.
    auto& curve = editor.getCurve();
    phazite::test::Mouse curveMouse (curve);
    const auto centre = curve.getLocalBounds().getCentre();
    curveMouse.press (centre);
    curveMouse.dragTo (centre - juce::Point<int> (0, 10));
    curveMouse.dragTo (centre - juce::Point<int> (0, 30));
    curveMouse.release();
    CHECK (song.valueOf ("drive") > 6.5f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Drive"));
}

TEST_CASE ("The delay's editor drags its first echo for the time and feedback, and its band's ends for the cuts")
{
    Song song (Delay::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::DelayEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::DelayEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, Delay::info);
    phazite::test::checkChildrenApart (editor);
    phazite::test::checkChildrenApart (editor.getEchoes());

    // The first echo dragged to where the second was: a note value twice as long (1/4 from 1/8), and higher, more feedback.
    auto& echoes = editor.getEchoes();
    phazite::test::Mouse mouse (echoes);
    const auto first = editor.getEchoPosition (1).roundToInt(), second = editor.getEchoPosition (2).roundToInt();
    const auto timeBefore = song.valueOf ("time");
    mouse.press (first - juce::Point<int> (0, 10));
    mouse.dragTo (second - juce::Point<int> (0, 60));
    mouse.release();
    CHECK (song.valueOf ("time") > timeBefore);
    CHECK (song.valueOf ("feedback") > 0.35f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Time"));
    phazite::undo (*song.edit);
    CHECK (song.valueOf ("time") == doctest::Approx (timeBefore));
    CHECK (song.valueOf ("feedback") == doctest::Approx (0.35f));

    // The band's high end dragged left: a lower high cut.
    mouse.pause();
    const auto band = editor.getBandArea();
    const auto highEnd = juce::Point<int> (band.getX() + band.getWidth() * 3 / 4, band.getCentreY());
    mouse.press (highEnd);
    mouse.dragTo (highEnd - juce::Point<int> (20, 0));
    mouse.release();
    CHECK (song.valueOf ("highCut") < 8000.0f);
    CHECK (song.valueOf ("lowCut") == doctest::Approx (100.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change High Cut"));
}

TEST_CASE ("The stereo expander's editor shows its image and how alike its sides are as it plays")
{
    Song song (phazite::devices::StereoExpander::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::StereoExpanderEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::StereoExpanderEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::StereoExpander::info);
    phazite::test::checkChildrenApart (editor);

    // Mono noise through its scope, as the audio thread hands it over: the image fills with dots, and the sides are alike.
    auto* scope = device.getScope();
    REQUIRE (scope != nullptr);
    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow(); // the editor listens
    REQUIRE (scope->isListening());

    std::vector<float> noise (512);
    juce::Random random (3);

    for (auto& sample : noise)
        sample = random.nextFloat() - 0.5f;

    scope->pushInput (noise.data(), noise.data(), (int) noise.size());
    scope->pushOutput (noise.data(), noise.data(), (int) noise.size());
    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow();
    CHECK (editor.getNumDots() == 512);
}

TEST_CASE ("The phaser's editor shows where the LFO has each side's notches, and its dot drags the frequency and feedback")
{
    Song song (phazite::devices::Phaser::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::PhaserEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::PhaserEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::Phaser::info);
    phazite::test::checkChildrenApart (editor);

    // Before it plays, its LFO at the start of its cycle: the left at the bottom of its sweep (800 Hz, two octaves down at
    // full depth, so 1.2 at 60 %), the right a quarter of a cycle ahead (Stereo at 50 %), in the middle.
    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow();
    CHECK (editor.getShownCentre (0) == doctest::Approx (800.0 * std::exp2 (-1.2)).epsilon (0.001));
    CHECK (editor.getShownCentre (1) == doctest::Approx (800.0).epsilon (0.001));

    // The dot dragged left and up: a lower frequency and more feedback, one step named after the frequency.
    auto& picture = editor.getPicture();
    phazite::test::Mouse mouse (picture);
    const auto dot = editor.getDotPosition().roundToInt();
    mouse.press (dot);
    mouse.dragTo (dot - juce::Point<int> (40, 0));
    mouse.dragTo (dot - juce::Point<int> (60, 30));
    mouse.release();
    CHECK (song.valueOf ("frequency") < 600.0f);
    CHECK (song.valueOf ("feedback") > 0.4f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Frequency"));
    phazite::undo (*song.edit);
    CHECK (song.valueOf ("frequency") == doctest::Approx (800.0f));
    CHECK (song.valueOf ("feedback") == doctest::Approx (0.3f));

    // The wheel turns the depth.
    mouse.pause();
    mouse.wheel (picture.getLocalBounds().getCentre(), 0.5f);
    CHECK (song.valueOf ("depth") > 0.6f);
}

TEST_CASE ("The flanger's editor shows how far each side's copy lags, and its dot drags the delay and feedback")
{
    Song song (phazite::devices::Flanger::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::FlangerEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::FlangerEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::Flanger::info);
    phazite::test::checkChildrenApart (editor);

    // Its triangle at the start of its cycle: the left at the bottom of its sweep (3 ms, 3.5 octaves shorter at 70 %), the
    // right a quarter of a cycle ahead, halfway.
    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow();
    CHECK (editor.getShownDelay (0) == doctest::Approx (3.0 * std::exp2 (-3.5)).epsilon (0.001));
    CHECK (editor.getShownDelay (1) == doctest::Approx (3.0 * std::exp2 (-1.75)).epsilon (0.001));

    // The dot at the delay's frequency (333 Hz, for 3 ms) dragged right and up: a shorter delay, more feedback, one step.
    auto& picture = editor.getPicture();
    phazite::test::Mouse mouse (picture);
    const auto dot = editor.getDotPosition().roundToInt();
    mouse.press (dot);
    mouse.dragTo (dot + juce::Point<int> (30, 0));
    mouse.dragTo (dot + juce::Point<int> (50, -30));
    mouse.release();
    CHECK (song.valueOf ("delay") < 2.5f);
    CHECK (song.valueOf ("feedback") > 0.4f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Delay"));

    // Dragged to the bottom: the feedback's sign turned, as far as it goes.
    mouse.pause();
    mouse.press (editor.getDotPosition().roundToInt());
    mouse.dragTo ({ editor.getDotPosition().roundToInt().x, picture.getHeight() });
    mouse.release();
    CHECK (song.valueOf ("feedback") == doctest::Approx (-1.0f));
}

TEST_CASE ("The vocoder's editor says what its carrier is, and a drag across its bands moves the formant as one undo step")
{
    Song song (phazite::devices::Vocoder::typeID);
    const auto device = phazite::makeDeviceHandle (*song.plugin);
    CHECK (dynamic_cast<phazite::ui::VocoderEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::VocoderEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::Vocoder::info);
    phazite::test::checkChildrenApart (editor);

    // The buzz at first, at C3; a track as the carrier, while none feeds its sidechain, says so.
    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow();
    CHECK (editor.getCarrierText() == juce::String ("Buzz C3"));
    song.plugin->getAutomatableParameterByID ("carrier")->setParameter (3.0f, juce::sendNotification);
    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow();
    CHECK (editor.getCarrierText() == juce::String ("Track: none feeds its sidechain"));

    // From the fifth band's column to the seventh's: the carrier's bands up by as far (10.6 semitones), one step.
    auto& bands = editor.getBands();
    phazite::test::Mouse mouse (bands);
    const auto from = editor.getColumnPosition (4).roundToInt() - juce::Point<int> (0, 20);
    const auto to = editor.getColumnPosition (6).roundToInt() - juce::Point<int> (0, 20);
    mouse.press (from);
    mouse.dragTo (from + juce::Point<int> (10, 0));
    mouse.dragTo (to);
    mouse.release();
    CHECK (std::abs (song.valueOf ("formant") - 12.0f * std::log2 (std::pow (100.0f, 2.0f / 15.0f))) < 0.8f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Formant"));
}
