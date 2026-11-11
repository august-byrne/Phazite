// SPDX-License-Identifier: AGPL-3.0-or-later
// The Filter's, the saturator's, the exciter's, the vinyl's, the delay's, the stereo expander's, the phaser's, the flanger's,
// the chorus's and the vocoder's editors (src/ui/devices/): a control for each parameter but the mix (the header's), what their
// pictures show, and the drags on them, each one undo step.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Chorus.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/saturation/Exciter.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
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
#include "ui/devices/ChorusEditor.h"
#include "ui/devices/DelayEditor.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DeviceWindowContent.h"
#include "ui/devices/ExciterEditor.h"
#include "ui/devices/FilterEditor.h"
#include "ui/devices/FlangerEditor.h"
#include "ui/devices/PhaserEditor.h"
#include "ui/devices/SaturatorEditor.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/devices/StereoExpanderEditor.h"
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

TEST_CASE ("The Filter's editor draws its response, and its dot drags the cutoff and resonance as one undo step")
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
}

TEST_CASE ("The exciter's editor drags its dot for the frequency and the amount, meters the band's reach, and listens to what is added")
{
    using phazite::devices::Exciter;
    Song song (Exciter::typeID);
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::ExciterEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::ExciterEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, Exciter::info);
    phazite::test::checkChildrenApart (editor);

    // The dot is at the frequency, as high as the top's lift. Dragged left and up: a lower frequency and more of the band
    // added, one step named after the frequency.
    auto& graph = editor.getGraph();
    CHECK (graph.getTitle() == juce::String ("Excited band"));
    phazite::test::Mouse mouse (graph);
    const auto dot = editor.getDotPosition().roundToInt();
    CHECK (graph.getLocalBounds().contains (dot));
    mouse.press (dot);
    mouse.dragTo (dot - juce::Point<int> (20, 0));
    mouse.dragTo (dot - juce::Point<int> (40, 30));
    mouse.release();
    CHECK (song.valueOf ("frequency") < 4000.0f);
    CHECK (song.valueOf ("frequency") > 1000.0f);
    CHECK (song.valueOf ("amount") > 0.6f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Frequency"));
    CHECK (editor.getDotPosition().x == doctest::Approx ((float) dot.x - 40.0f).epsilon (0.02));
    phazite::undo (*song.song);
    CHECK (song.valueOf ("frequency") == doctest::Approx (5000.0f));
    CHECK (song.valueOf ("amount") == doctest::Approx (0.35f));

    // The wheel turns the drive.
    mouse.pause();
    mouse.wheel (graph.getLocalBounds().getCentre(), 0.5f);
    CHECK (song.valueOf ("drive") > 18.5f);

    // The meter: how far the driven band reached into the curve, against where it bends, up at once and falling back.
    auto* exciter = device.getProcessor<Exciter>();
    REQUIRE (exciter != nullptr);
    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();
    ticker.tickNow();
    CHECK (editor.getShownReach() < -20.0f);
    exciter->getReach().raise (2.0f);
    ticker.tickNow();
    CHECK (editor.getShownReach() == doctest::Approx (6.02f).epsilon (0.01));
    ticker.tickNow();
    CHECK (editor.getShownReach() < 6.0f);
    CHECK (editor.getShownReach() > 4.0f);

    // Its displays listen to what goes in and out, and to what is added.
    REQUIRE (device.getScope() != nullptr);
    CHECK (device.getScope()->isListening());
    CHECK (exciter->getAddedTap().isListening());

    // Listen: only while it is on does the exciter play what it adds alone.
    auto& listen = editor.getListenButton();
    CHECK (listen.getTitle() == juce::String ("Listen"));
    CHECK_FALSE (exciter->isPlayingAdded());
    phazite::test::Mouse listenMouse (listen);
    listenMouse.click (listen.getLocalBounds().getCentre());
    CHECK (listen.getToggleState());
    ticker.tickNow();
    CHECK (exciter->isPlayingAdded());
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

TEST_CASE ("The stereo expander's editor shows its image and how alike its sides are as it plays")
{
    Song song (phazite::devices::StereoExpander::typeID);
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::StereoExpanderEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::StereoExpanderEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::StereoExpander::info);
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
}

TEST_CASE ("The phaser's editor shows where the LFO has each side's notches, and its dot drags the frequency and feedback")
{
    Song song (phazite::devices::Phaser::typeID);
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::PhaserEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::PhaserEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::Phaser::info);
    phazite::test::checkChildrenApart (editor);

    // Before it plays, its LFO at the start of its cycle: the left at the bottom of its sweep (800 Hz, two octaves down at
    // full depth, so 1.2 at 60 %), the right a quarter of a cycle ahead (Stereo at 50 %), in the middle.
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
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
    phazite::undo (*song.song);
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
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::FlangerEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::FlangerEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, phazite::devices::Flanger::info);
    phazite::test::checkChildrenApart (editor);

    // Its triangle at the start of its cycle: the left at the bottom of its sweep (3 ms, 3.5 octaves shorter at 70 %), the
    // right a quarter of a cycle ahead, halfway.
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
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

TEST_CASE ("The chorus's editor shows how far each copy lags, three a side as an ensemble, and its dot drags the delay and depth")
{
    using phazite::devices::Chorus;
    Song song (Chorus::typeID);
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::ChorusEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::ChorusEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    checkEveryParameterButMixHasAControl (editor, Chorus::info);
    phazite::test::checkChildrenApart (editor);

    // Its triangle at the start of its cycle: the left side's copy at the bottom of its sweep (3.5 ms, half of it shorter),
    // the right side's half a cycle ahead, at the top.
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getShownVoices() == 1);
    CHECK (editor.getShownDelay (0, 0) == doctest::Approx (1.75).epsilon (0.001));
    CHECK (editor.getShownDelay (1, 0) == doctest::Approx (5.25).epsilon (0.001));

    // As the string machines' ensemble, three copies a side, each a third of a cycle after the one before, with the fast
    // sine on it.
    phazite::setValueSetByHand (*song.song, phazite::ParameterRef { song.device, "voices" }, 2.0f);
    phazite::setValueSetByHand (*song.song, phazite::ParameterRef { song.device, "shimmer" }, 1.0f);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getShownVoices() == 3);
    std::vector<float> values;

    for (const auto& parameter : Chorus::info.parameters)
        values.push_back (parameter.defaultValue);

    values[Chorus::voices] = 2.0f;
    values[Chorus::shimmer] = 1.0f;

    for (int voice = 0; voice < 3; ++voice)
        CHECK (editor.getShownDelay (0, voice) == doctest::Approx (Chorus::getVoiceDelay (values, voice / 3.0, voice)).epsilon (0.001));

    phazite::setValueSetByHand (*song.song, phazite::ParameterRef { song.device, "voices" }, 0.0f);
    phazite::setValueSetByHand (*song.song, phazite::ParameterRef { song.device, "shimmer" }, 0.0f);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();

    // The dot at the delay's frequency (286 Hz, for 3.5 ms) dragged right and up: a shorter delay, more depth, one step.
    auto& picture = editor.getPicture();
    phazite::test::Mouse mouse (picture);
    const auto dot = editor.getDotPosition().roundToInt();
    mouse.press (dot);
    mouse.dragTo (dot + juce::Point<int> (30, 0));
    mouse.dragTo (dot + juce::Point<int> (50, -30));
    mouse.release();
    CHECK (song.valueOf ("delay") < 3.0f);
    CHECK (song.valueOf ("depth") > 0.6f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Delay"));

    // The wheel turns the rate faster.
    mouse.pause();
    mouse.wheel (picture.getLocalBounds().getCentre(), 0.5f);
    CHECK (song.valueOf ("rate") > 0.5f);
}

TEST_CASE ("The chorus's editor shows each voice in its part of the sweep, and Phase and Spread faint while it has one voice")
{
    using phazite::devices::Chorus;
    Song song (Chorus::typeID);
    const auto device = song.getDevice();
    phazite::ui::ChorusEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();
    const auto set = [&song] (const char* id, float value)
    { phazite::setValueSetByHand (*song.song, phazite::ParameterRef { song.device, id }, value); };

    // A control for each of the voices' parameters, each with its title: the count a list, the others knobs.
    for (const auto& [id, title] : { std::pair<const char*, const char*> { "voices", "Voices" },
                                     { "voicePhase", "Voice Phase" },
                                     { "spread", "Spread" },
                                     { "shimmer", "Shimmer" } })
    {
        CAPTURE (id);
        auto* control = editor.findControl (id);
        REQUIRE (control != nullptr);
        CHECK (control->isShowing());
        CHECK (control->getTitle() == juce::String (title));
    }

    CHECK (dynamic_cast<juce::ComboBox*> (editor.findControl ("voices")) != nullptr);
    CHECK (dynamic_cast<juce::Slider*> (editor.findControl ("spread")) != nullptr);

    // One voice a side has no other to keep a phase from or to share the sweep with: those two faint, the others not.
    ticker.tickNow();
    CHECK (editor.findControl ("voicePhase")->getAlpha() < 0.9f);
    CHECK (editor.findControl ("spread")->getAlpha() < 0.9f);
    CHECK (editor.findControl ("shimmer")->getAlpha() == doctest::Approx (1.0f));
    CHECK (editor.findControl ("voices")->getAlpha() == doctest::Approx (1.0f));

    // Four voices a side: Phase and Spread lit, four copies shown each side, all on one line while they share the sweep.
    set ("voices", 3.0f);
    ticker.tickNow();
    CHECK (editor.findControl ("voicePhase")->getAlpha() == doctest::Approx (1.0f));
    CHECK (editor.findControl ("spread")->getAlpha() == doctest::Approx (1.0f));
    CHECK (editor.getShownVoices() == 4);
    CHECK (editor.getHeadPosition (0, 0).y == doctest::Approx (editor.getHeadPosition (0, 3).y));

    // Spread gives each its part of the sweep: each copy shown within its own part, the later copies' longer, each on a line
    // of its own under the one before, all within the picture.
    set ("spread", 0.62f);
    set ("delay", 8.2f);
    set ("depth", 0.61f);
    ticker.tickNow();
    std::vector<float> values;

    for (const auto& parameter : Chorus::info.parameters)
        values.push_back (song.valueOf (std::string (parameter.id).c_str()));

    for (int channel = 0; channel < 2; ++channel)
    {
        for (int voice = 0; voice < 4; ++voice)
        {
            CAPTURE (channel);
            CAPTURE (voice);
            const auto reach = Chorus::getVoiceReach (values, voice);
            CHECK (editor.getShownDelay (channel, voice) >= reach.shortest - 0.001);
            CHECK (editor.getShownDelay (channel, voice) <= reach.longest + 0.001);
            CHECK (editor.getPicture().getLocalBounds().toFloat().contains (editor.getHeadPosition (channel, voice)));

            if (voice > 0)
            {
                CHECK (editor.getHeadPosition (channel, voice).y > editor.getHeadPosition (channel, voice - 1).y + 1.0f);
                CHECK (Chorus::getVoiceReach (values, voice).shortest > Chorus::getVoiceReach (values, voice - 1).shortest);
            }
        }
    }

    // Eight a side, the most: all shown, still within the picture; and the editor is as wide as with one, so that it fits
    // the dock the same.
    const auto width = editor.getWidth();
    set ("voices", 7.0f);
    ticker.tickNow();
    CHECK (editor.getShownVoices() == Chorus::mostVoices);

    for (int voice = 0; voice < Chorus::mostVoices; ++voice)
        CHECK (editor.getPicture().getLocalBounds().toFloat().contains (editor.getHeadPosition (1, voice)));

    CHECK (editor.getWidth() == width);
    phazite::test::checkChildrenApart (editor);

    // The picture repaints when a voice's control turns, without sound.
    phazite::test::dispatchPendingMessages();
    window.takeRepaintedArea();
    set ("voicePhase", 0.3f);
    ticker.tickNow();
    CHECK_FALSE (window.takeRepaintedArea().isEmpty());
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
