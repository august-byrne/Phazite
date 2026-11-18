// SPDX-License-Identifier: AGPL-3.0-or-later
// The Filter's, the saturator's, the vinyl's, the delay's, Stereo's and the vocoder's editors
// (src/ui/devices/): a control for each parameter but the mix (the header's), what their pictures show, and the drags on
// them, each one undo step. Motion's editor is in MotionTests.cpp.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/Stereo.h"
#include "devices/synth/Synth.h"
#include "devices/vinyl/Vinyl.h"
#include "devices/vocoder/Vocoder.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DelayEditor.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DeviceWindowContent.h"
#include "ui/devices/FilterEditor.h"
#include "ui/devices/SaturatorEditor.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/devices/StereoEditor.h"
#include "ui/devices/VinylEditor.h"
#include "ui/devices/VocoderEditor.h"

namespace
{
using phazite::devices::Delay;
using phazite::devices::FilterEffect;
using phazite::devices::Saturator;

// A device on a track of its own song, open in a context of its own whose session plays it.
struct Song
{
    explicit Song (const char* typeID)
    {
        phazite::setSong (context, song.get());
        device = phazite::insertDeviceState (*song, phazite::getTrackIDs (*song)[0], phazite::makeOwnDeviceState (*song, typeID));
        REQUIRE (device.isValid());
        phazite::getSession (context)->update();
        song->getUndoManager().clearUndoHistory();
    }

    ~Song() { phazite::setSong (context, nullptr); }

    phazite::DeviceHandle getDevice() const { return phazite::makeDeviceHandle (*song, device); }
    float valueOf (const char* id) const { return phazite::getValueSetByHand (*song, phazite::ParameterRef { device, id }); }
    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::DeviceID device;
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

TEST_CASE ("The Filter's editor has a control for each parameter but the mix, and its dot drags the cutoff and resonance as one undo step")
{
    Song song (FilterEffect::typeID);
    const auto device = song.getDevice();
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
    phazite::undo (*song.song);
    CHECK (song.valueOf ("cutoff") == doctest::Approx (FilterEffect::info.parameters[FilterEffect::cutoff].defaultValue));
    CHECK (song.valueOf ("resonance") == doctest::Approx (0.1f));

    // The wheel turns the drive.
    mouse.wheel (middle, 0.5f);
    CHECK (song.valueOf ("drive") > 0.5f);
}

TEST_CASE ("The Filter's graph runs from 20 Hz to 20 kHz evenly by octaves across, and the resonance from none to all up it")
{
    // The axes as the graph says it draws them, apart from its own code: its curve inside its edge's pixel, 20 Hz at the
    // left, 20 kHz at the right, each octave as wide as the next; the resonance from 0 at the bottom to 1 at the top.
    Song song (FilterEffect::typeID);
    phazite::ui::FilterEditor editor (song.getDevice());
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& graph = editor.getGraph();
    const auto area = graph.getLocalBounds().toFloat().reduced (1.0f);
    const auto frequencyAt = [area] (int x) { return 20.0 * std::pow (1000.0, ((double) x - area.getX()) / area.getWidth()); };
    const auto resonanceAt = [area] (int y) { return (area.getBottom() - (double) y) / area.getHeight(); };

    // A drag of the dot toward the pixel nearest 1 kHz at half the resonance, and one nearest 100 Hz near the top: each moves
    // the dot as far as the pointer went (from where the dot was: known bug 116), and lands on the values where it is drawn.
    // A press away from the dot puts it under the pointer.
    for (const auto& [hz, resonance] : { std::pair (1000.0, 0.5), std::pair (100.0, 0.9) })
    {
        CAPTURE (hz);
        const juce::Point<int> target (juce::roundToInt (area.getX() + std::log (hz / 20.0) / std::log (1000.0) * area.getWidth()),
                                       juce::roundToInt (area.getBottom() - resonance * area.getHeight()));
        phazite::test::Mouse mouse (graph);
        const auto dot = editor.getDotPosition();
        const auto pressed = dot.roundToInt();
        mouse.press (pressed);
        mouse.dragTo (target);
        mouse.release();
        mouse.pause();
        const auto landed = dot + (target - pressed).toFloat();

        CHECK (song.valueOf ("cutoff")
               == doctest::Approx (20.0 * std::pow (1000.0, (landed.x - area.getX()) / area.getWidth())).epsilon (0.002));
        CHECK (song.valueOf ("cutoff") == doctest::Approx (hz).epsilon (0.03)); // a pixel is under 3 % of a frequency
        CHECK (song.valueOf ("resonance") == doctest::Approx ((area.getBottom() - landed.y) / area.getHeight()).epsilon (0.002));
        CHECK (editor.getDotPosition().getDistanceFrom (landed) < 0.05f);

        // Pressed away from it, and let go: the dot under the pointer, at the values there.
        const juce::Point<int> away (juce::roundToInt (area.getX() + area.getWidth() * 0.2f),
                                     juce::roundToInt (area.getBottom() - area.getHeight() * 0.3f));
        mouse.press (away);
        mouse.release();
        mouse.pause();
        CHECK (song.valueOf ("cutoff") == doctest::Approx (frequencyAt (away.x)).epsilon (0.002));
        CHECK (song.valueOf ("resonance") == doctest::Approx (resonanceAt (away.y)).epsilon (0.002));
    }
}

TEST_CASE ("The saturator's editor picks its shape with buttons drawn as their curves, and shows how far the sound reaches")
{
    Song song (Saturator::typeID);
    const auto device = song.getDevice();
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

    // Six shapes, Crush last.
    CHECK (editor.getShapeButton (5).getTitle() == juce::String ("Crush"));
    phazite::test::Mouse crushMouse (editor.getShapeButton (5));
    crushMouse.click (editor.getShapeButton (5).getLocalBounds().getCentre());
    CHECK (song.valueOf ("shape") == doctest::Approx (5.0f));
}

TEST_CASE ("The saturator's editor switches its Band at its title, drags the band's edges, and listens to the band while it shows")
{
    Song song (Saturator::typeID);
    const auto device = song.getDevice();
    phazite::ui::SaturatorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();

    // The Band's switch, at its part's title: off at first, a click turns it on as one undo step.
    auto* bandSwitch = dynamic_cast<juce::Button*> (editor.findControl ("band"));
    REQUIRE (bandSwitch != nullptr);
    CHECK_FALSE (bandSwitch->getToggleState());
    phazite::test::Mouse switchMouse (*bandSwitch);
    switchMouse.click (bandSwitch->getLocalBounds().getCentre());
    CHECK (song.valueOf ("band") == doctest::Approx (1.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Band"));

    // Its picture shows the band from 3 kHz to the top: its low edge dragged left lowers From alone, as one undo step, and
    // its high edge, at the right, dragged left lowers To.
    auto& view = editor.getBandView();
    const auto y = view.getHeight() / 2;
    phazite::test::Mouse mouse (view);
    const juce::Point<int> from (juce::roundToInt (editor.getEdgeX (false)), y), to (juce::roundToInt (editor.getEdgeX (true)) - 1, y);
    CHECK (view.getLocalBounds().contains (from));
    CHECK (view.getLocalBounds().contains (to));
    mouse.press (from);
    mouse.dragTo (from - juce::Point<int> (15, 0));
    mouse.dragTo (from - juce::Point<int> (30, 0));
    mouse.release();
    CHECK (song.valueOf ("bandLow") < 2500.0f);
    CHECK (song.valueOf ("bandLow") > 500.0f);
    CHECK (song.valueOf ("bandHigh") == doctest::Approx (20000.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change From"));
    ticker.tickNow();
    CHECK (editor.getEdgeX (false) == doctest::Approx ((float) from.x - 30.0f).epsilon (0.02));
    phazite::undo (*song.song);
    CHECK (song.valueOf ("bandLow") == doctest::Approx (3000.0f));

    mouse.pause();
    mouse.press (to);
    mouse.dragTo (to - juce::Point<int> (40, 0));
    mouse.release();
    CHECK (song.valueOf ("bandHigh") < 15000.0f);
    CHECK (song.valueOf ("bandLow") == doctest::Approx (3000.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change To"));
    phazite::undo (*song.song);

    // To set under an octave above From plays an octave above it, where its edge is drawn: a drag of that edge starts there,
    // not from the knob's value, and stops an octave above From; after it, To's knob shows what plays.
    const auto setByHand = [&] (const char* id, float value)
    {
        phazite::setValueSetByHand (*song.song, phazite::ParameterRef { song.device, id }, value);
        ticker.tickNow();
    };

    setByHand ("bandHigh", 300.0f);
    const auto drawnTo = editor.getEdgeX (true);
    CHECK (drawnTo > editor.getEdgeX (false) + 10.0f); // 6 kHz, an octave above From
    const juce::Point<int> toEdge (juce::roundToInt (drawnTo) - 1, y);
    mouse.pause();
    mouse.press (toEdge);
    mouse.dragTo (toEdge + juce::Point<int> (5, 0));
    mouse.dragTo (toEdge + juce::Point<int> (10, 0));
    mouse.release();
    CHECK (song.valueOf ("bandHigh") > 6000.0f);
    CHECK (song.valueOf ("bandHigh") < 12000.0f);
    CHECK (song.describeUndo() == juce::String ("Undo Change To"));
    ticker.tickNow();
    CHECK (editor.getEdgeX (true) == doctest::Approx (drawnTo + 10.0f).epsilon (0.02));

    mouse.pause();
    const juce::Point<int> raisedTo (juce::roundToInt (editor.getEdgeX (true)) - 1, y);
    mouse.press (raisedTo);
    mouse.dragTo (raisedTo - juce::Point<int> (100, 0));
    mouse.dragTo (raisedTo - juce::Point<int> (200, 0));
    mouse.release();
    CHECK (song.valueOf ("bandHigh") == doctest::Approx (6000.0f));

    // A drag of From up pushes To ahead of it, an octave above, in the same undo step; brought back down in the same drag,
    // From lets To back to where it was.
    setByHand ("bandHigh", 8000.0f);
    const juce::Point<int> fromEdge (juce::roundToInt (editor.getEdgeX (false)), y);
    mouse.pause();
    mouse.press (fromEdge);
    mouse.dragTo (fromEdge + juce::Point<int> (40, 0));
    mouse.dragTo (fromEdge + juce::Point<int> (80, 0));
    CHECK (song.valueOf ("bandLow") > 4000.0f);
    CHECK (song.valueOf ("bandHigh") == doctest::Approx (2.0f * song.valueOf ("bandLow")));
    mouse.dragTo (fromEdge + juce::Point<int> (4, 0));
    mouse.release();
    CHECK (song.valueOf ("bandLow") < 4000.0f);
    CHECK (song.valueOf ("bandHigh") == doctest::Approx (8000.0f));

    mouse.pause();
    const juce::Point<int> movedFrom (juce::roundToInt (editor.getEdgeX (false)), y);
    mouse.press (movedFrom);
    mouse.dragTo (movedFrom + juce::Point<int> (40, 0));
    mouse.dragTo (movedFrom + juce::Point<int> (80, 0));
    mouse.release();
    const auto pushedFrom = song.valueOf ("bandLow");
    CHECK (pushedFrom > 4000.0f);
    CHECK (song.valueOf ("bandHigh") == doctest::Approx (2.0f * pushedFrom));
    CHECK (song.describeUndo() == juce::String ("Undo Change From"));
    phazite::undo (*song.song); // both, as one step
    CHECK (song.valueOf ("bandLow") < 4000.0f);
    CHECK (song.valueOf ("bandHigh") == doctest::Approx (8000.0f));

    // Its picture listens to what goes in and out.
    REQUIRE (device.getScope() != nullptr);
    ticker.tickNow();
    CHECK (device.getScope()->isListening());

    // Listen: only while it is on, and the editor shows, does the saturator play the band alone; hidden, the editor asks no
    // more, and the sound comes back half a second later.
    auto* saturator = device.getProcessor<Saturator>();
    REQUIRE (saturator != nullptr);
    phazite::test::HeldClock clock;
    auto& listen = editor.getListenButton();
    CHECK (listen.getTitle() == juce::String ("Listen"));
    ticker.tickNow();
    CHECK_FALSE (saturator->isPlayingBand());
    phazite::test::Mouse listenMouse (listen);
    listenMouse.click (listen.getLocalBounds().getCentre());
    CHECK (listen.getToggleState());
    ticker.tickNow();
    CHECK (saturator->isPlayingBand());

    editor.setVisible (false);
    clock.advance (501.0);
    ticker.tickNow();
    CHECK_FALSE (saturator->isPlayingBand());
}

TEST_CASE ("The vinyl's editor turns its record as it plays, draws its pitch and its crackles, and drags for the wow and the band")
{
    using phazite::devices::Vinyl;
    Song song (Vinyl::typeID);
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::VinylEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::VinylEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, Vinyl::info);
    phazite::test::checkChildrenApart (editor);
    phazite::test::checkChildrenApart (editor.getRecord());

    // A drag up the record: more wow, one step named after it.
    auto& record = editor.getRecord();
    CHECK (record.getTitle() == juce::String ("Record"));
    phazite::test::Mouse mouse (record);
    const auto centre = editor.getRecordCentre().roundToInt();
    CHECK (record.getLocalBounds().contains (centre));
    mouse.press (centre);
    mouse.dragTo (centre - juce::Point<int> (0, 10));
    mouse.dragTo (centre - juce::Point<int> (0, 40));
    mouse.release();
    CHECK (song.valueOf ("wow") > 8.0f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Wow"));
    phazite::undo (*song.song);
    CHECK (song.valueOf ("wow") == doctest::Approx (4.0f));

    // The wheel turns the record faster.
    mouse.pause();
    mouse.wheel (centre, 0.5f);
    CHECK (song.valueOf ("wowRate") > 0.56f);

    // The band's high end dragged left: a lower high cut, the low cut as it was.
    mouse.pause();
    const auto band = editor.getBandArea();
    const auto highEnd = juce::Point<int> (band.getRight() - 4, band.getCentreY());
    mouse.press (highEnd);
    mouse.dragTo (highEnd - juce::Point<int> (20, 0));
    mouse.dragTo (highEnd - juce::Point<int> (band.getWidth() / 3, 0));
    mouse.release();
    CHECK (song.valueOf ("highCut") < 8000.0f);
    CHECK (song.valueOf ("lowCut") == doctest::Approx (30.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Change High Cut"));

    // As it plays, crackling densely: the record turns, its pitch's history fills, and a speck comes where it crackled.
    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();
    ticker.tickNow(); // the editor listens
    auto* vinyl = device.getProcessor<Vinyl>();
    REQUIRE (vinyl != nullptr);
    CHECK (vinyl->getPitchTap().isListening());
    vinyl->prepare (48000.0, 480);
    std::vector<float> values;

    for (const auto& parameter : Vinyl::info.parameters)
        values.push_back (song.valueOf (parameter.id.data())); // the IDs are literals

    values[Vinyl::wow] = 20.0f;
    values[Vinyl::crackle] = -12.0f;
    values[Vinyl::density] = 200.0f;
    juce::AudioBuffer<float> buffer (2, 480);

    for (int block = 0; block < 50; ++block)
    {
        buffer.clear();
        phazite::devices::ProcessContext context;
        context.channels = buffer.getArrayOfWritePointers();
        context.numChannels = 2;
        context.numSamples = buffer.getNumSamples();
        context.parameters = values;
        context.isPlaying = true;
        context.beat = block * 480 * 2.0 / 48000.0;
        vinyl->process (context);
    }

    ticker.tickNow();
    CHECK (editor.getShownTurn() == doctest::Approx (0.5 * values[Vinyl::wowRate]).epsilon (0.02)); // half a second's turn
    CHECK (std::abs (editor.getShownPitch()) <= 26.0f);
    CHECK (editor.getNumReadings() >= 40);
    CHECK (editor.getNumSpecks() > 0);
}

TEST_CASE ("The delay's editor drags its first echo for the time and feedback, and its band's ends for the cuts")
{
    Song song (Delay::typeID);
    const auto device = song.getDevice();
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
    CHECK (song.describeUndo() == juce::String ("Undo Change Note")); // the time as a note value, while synced
    phazite::undo (*song.song);
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

TEST_CASE ("Stereo's editor shows its image and how alike its sides are as it plays")
{
    Song song (phazite::devices::Stereo::typeID);
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::StereoEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::StereoEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::Stereo::info);
    phazite::test::checkChildrenApart (editor);

    // Mono noise through its scope, as the audio thread hands it over: the image fills with dots, and the sides are alike.
    auto* scope = device.getScope();
    REQUIRE (scope != nullptr);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow(); // the editor listens
    REQUIRE (scope->isListening());

    std::vector<float> noise (512);
    juce::Random random (3);

    for (auto& sample : noise)
        sample = random.nextFloat() - 0.5f;

    scope->pushInput (noise.data(), noise.data(), (int) noise.size());
    scope->pushOutput (noise.data(), noise.data(), (int) noise.size());
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getNumDots() == 512);
    CHECK (std::abs (editor.getDotsAcross()) < 0.01f); // mono stands straight up

    // Known bug 115: a sound on the left alone lies along the L line, left of the centre; on the right alone, right of it.
    const std::vector<float> silence (noise.size(), 0.0f);
    scope->pushOutput (noise.data(), silence.data(), (int) noise.size());
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getDotsAcross() < -0.05f);

    scope->pushOutput (silence.data(), noise.data(), (int) noise.size());
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getDotsAcross() > 0.05f);
}

TEST_CASE ("Stereo's editor shows its picture and its four parts at once in the default dock, each control titled")
{
    Song song (phazite::devices::Stereo::typeID);
    phazite::ui::StereoEditor editor (song.getDevice());
    constexpr int dockWidth = 552, dockHeight = 268; // beside the Devices view's column
    REQUIRE (editor.getMinimumHeightAtWidth (dockWidth) <= dockHeight);
    phazite::test::TestWindow window (editor, dockWidth, dockHeight);
    CHECK_FALSE (editor.getLayout().usesPages());

    for (int column = 0; column < 3; ++column)
        CHECK_FALSE (editor.getLayout().usesTabs (column));

    CHECK (editor.getImage().isShowing());

    for (const auto& parameter : phazite::devices::Stereo::info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        auto* control = editor.findControl (parameter.id);
        REQUIRE (control != nullptr);
        CHECK (control->isShowing());
        CHECK (control->getTitle().isNotEmpty());
        CHECK (editor.getLocalBounds().contains (editor.getLocalArea (control, control->getLocalBounds())));
    }

    // The new switches by the names the parts give them.
    CHECK (editor.findControl ("swap")->getTitle() == "Swap Sides");
    CHECK (editor.findControl ("dcFilter")->getTitle() == "DC Filter");
}

TEST_CASE ("The vocoder's editor says what its carrier is, and a drag across its bands moves the formant as one undo step")
{
    Song song (phazite::devices::Vocoder::typeID);
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::VocoderEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::VocoderEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::Vocoder::info);
    phazite::test::checkChildrenApart (editor);

    // The buzz at first, at C3; a track as the carrier, while none feeds its sidechain, says so.
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getCarrierText() == juce::String ("Buzz C3"));
    phazite::setValueSetByHand (*song.song, phazite::ParameterRef { song.device, "carrier" }, 3.0f);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getCarrierText() == juce::String ("Track: pick one by the carrier"));

    // Over the bands, a drag shows itself (the owner, 2026-11-10: "it should be obvious that you are able to do this
    // gesture"): a left-and-right arrow, the lines from the marks to the columns lit, and the hint bar saying what it does.
    auto& bands = editor.getBands();
    phazite::test::Mouse mouse (bands);
    const auto from = editor.getColumnPosition (4).roundToInt() - juce::Point<int> (0, 20);
    const auto to = editor.getColumnPosition (6).roundToInt() - juce::Point<int> (0, 20);
    CHECK_FALSE (editor.isBandsLit());
    mouse.moveTo (from);
    CHECK (editor.isBandsLit());
    CHECK (bands.getMouseCursor() == juce::MouseCursor::LeftRightResizeCursor);
    auto* tip = dynamic_cast<juce::TooltipClient*> (&bands);
    REQUIRE (tip != nullptr);
    CHECK (tip->getTooltip().contains ("Drag sideways"));

    // From the fifth band's column to the seventh's: the carrier's bands up by as far (10.6 semitones), one step.
    mouse.press (from);
    mouse.dragTo (from + juce::Point<int> (10, 0));
    mouse.dragTo (to);
    mouse.release();
    CHECK (std::abs (song.valueOf ("formant") - 12.0f * std::log2 (std::pow (100.0f, 2.0f / 15.0f))) < 0.8f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Formant"));
}

TEST_CASE ("The vocoder's carrier track is picked beside its carrier: the track chosen plays as its carrier, as one undo step")
{
    // Given the project, as the Devices view and its window show it, the vocoder picks the track that carries beside the
    // carrier's controls, the one place to choose what its sidechain listens to.
    Song song (phazite::devices::Vocoder::typeID);
    const auto tracks = phazite::ensureTracks (*song.song, 2);
    phazite::setTrackName (*song.song, tracks[1], "Synth");
    song.song->getUndoManager().clearUndoHistory();
    auto shown = phazite::ui::createDeviceEditor (song.context, song.device);
    auto* editor = dynamic_cast<phazite::ui::VocoderEditor*> (shown.get());
    REQUIRE (editor != nullptr);
    phazite::test::TestWindow window (*editor, editor->getWidth(), editor->getHeight());
    phazite::test::checkChildrenApart (*editor);

    // Level with the carrier's list, None at first.
    auto* picker = editor->getTrackPicker();
    REQUIRE (picker != nullptr);
    CHECK (picker->isShowing());
    CHECK (picker->getTitle() == juce::String ("Carrier track"));
    CHECK (picker->getButtonText() == juce::String ("None"));
    auto* list = editor->findControl ("carrier");
    REQUIRE (list != nullptr);
    CHECK (std::abs (picker->getScreenBounds().getCentreY() - list->getScreenBounds().getCentreY()) <= 1);
    CHECK (picker->getScreenX() >= list->getScreenBounds().getRight());

    // The synth: the vocoder listens to it, and plays it, all one step.
    REQUIRE (phazite::test::runMenuItem (picker->getMenu(), { "Synth" }));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getSidechainSource (*song.song, song.device) == tracks[1]);
    CHECK (juce::roundToInt (song.valueOf ("carrier")) == (int) phazite::devices::Vocoder::Carrier::track);
    CHECK (song.describeUndo() == juce::String ("Undo Sidechain from Synth"));
    CHECK (picker->getButtonText() == juce::String ("Synth"));
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor->getCarrierText() == juce::String ("Track: Synth"));

    // Undone: the buzz again, and the picker says None once the editor looks.
    phazite::undo (*song.song);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (picker->getButtonText() == juce::String ("None"));
    CHECK (editor->getCarrierText() == juce::String ("Buzz C3"));
}

TEST_CASE ("A built-in device's editor in a window of its own wraps its parts to the window's width, and scrolls")
{
    Song song (phazite::devices::Synth::typeID);
    phazite::ui::DeviceWindowContent content (song.getDevice());
    REQUIRE (content.hasEditor());
    const auto wide = content.getWidth(), high = content.getHeight();
    phazite::test::TestWindow window (content, wide, high);
    auto* editor = content.getEditor();
    REQUIRE (editor != nullptr);
    const auto tallest = editor->getHeight();

    // Half as wide: the synth's columns wrap into rows, taller than the window, which scrolls to them.
    content.setSize (wide / 2, high);
    CHECK (editor->getWidth() <= wide / 2);
    CHECK (editor->getHeight() > tallest);
    CHECK (content.getMinimumHeight() > 0);
    phazite::test::checkChildrenApart (*editor);

    // As narrow as the window can be: its columns one under another, each whole.
    CHECK (content.getMinimumWidth() < wide / 2);
    content.setSize (content.getMinimumWidth(), high);
    phazite::test::checkChildrenApart (*editor);

    for (auto* child : editor->getChildren())
        if (child->isVisible())
            CHECK (editor->getLocalBounds().contains (child->getBounds()));
}

TEST_CASE ("A built-in device's editor that doesn't wrap narrows its window only as far as its graph narrows, beside its controls")
{
    Song song (phazite::devices::Equalizer::typeID);
    phazite::ui::DeviceWindowContent content (song.getDevice());
    REQUIRE (content.hasEditor());
    const auto wide = content.getWidth(), high = content.getHeight();
    CHECK (content.getMinimumWidth() < wide);
    CHECK (content.getMinimumWidth() > phazite::ui::GraphTint::minimumWidth);

    // At its least, the graph narrowed beside the band's controls, each whole.
    phazite::test::TestWindow window (content, wide, high);
    content.setSize (content.getMinimumWidth(), high);
    auto* editor = content.getEditor();
    REQUIRE (editor != nullptr);
    phazite::test::checkChildrenApart (*editor);

    for (auto* child : editor->getChildren())
        if (child->isVisible())
            CHECK (editor->getLocalBounds().contains (child->getBounds()));
}
