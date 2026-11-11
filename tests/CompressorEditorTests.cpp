// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (src/ui/devices/CompressorEditor.h): its tabs, which are its modes, each its own page, a click on
// one an undo step; compressing, its characters' buttons, its knobs, its curve (the multiband's, src/ui/devices/DynamicsCurve.h)
// lit up to the level it hears, its meter both ways, and no key; ducking, its key picked over the key's cuts with Listen, its
// knobs, and its threshold a line to drag in its history; its history, two bars long at the song's tempo, on both pages; and
// each page whole in the default dock.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
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
#include "model/transport/internal/Tempo.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <vector>

namespace
{
using phazite::devices::Compressor;
namespace curves = phazite::devices::curves;

// Three tracks, Kick, Bass and Keys, with a compressor on the bass, open in a context of their own whose session plays
// them.
struct Song
{
    Song()
    {
        phazite::setSong (context, song.get());
        const auto tracks = phazite::ensureTracks (*song, 3);
        phazite::setTrackName (*song, tracks[0], "Kick");
        phazite::setTrackName (*song, tracks[1], "Bass");
        phazite::setTrackName (*song, tracks[2], "Keys");
        compressor = phazite::insertDeviceState (*song, tracks[1], phazite::makeOwnDeviceState (*song, Compressor::typeID));
        REQUIRE (compressor.isValid());
        phazite::getSession (context)->update();
        song->getUndoManager().clearUndoHistory();
    }

    ~Song() { phazite::setSong (context, nullptr); }

    phazite::DeviceHandle getDevice() const { return phazite::makeDeviceHandle (*song, compressor); }
    float valueOf (const char* id) const { return phazite::getValueSetByHand (*song, phazite::ParameterRef { compressor, id }); }
    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }
    phazite::TrackID getTrack (int index) const { return phazite::getTrackIDs (*song)[(size_t) index]; }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::DeviceID compressor;
};

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Compressor::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

// How far its curve turns a level down, at its default settings, once settled.
float getDefaultReduction (float decibels)
{
    const auto values = getDefaults();
    return decibels
           - 20.0f * std::log10 (curves::fromAxis (curves::getAt (Compressor::getCurve (values), curves::axisForDecibels (decibels))));
}

// Plays a steady level through the compressor's DSP, at its default settings or at the ones given, as the audio thread would.
void play (Compressor& compressor, float decibels, int numSamples, std::vector<float> values = getDefaults())
{
    juce::AudioBuffer<float> buffer (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
        for (int channel = 0; channel < 2; ++channel)
            buffer.setSample (channel, i, (i % 2 == 0 ? 1.0f : -1.0f) * juce::Decibels::decibelsToGain (decibels));

    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = numSamples;
    block.parameters = values;
    compressor.process (block);
}

// A menu open inside a window (as the app's open: ui/theme/Popups.h), or nullptr.
juce::Component* findMenuWindow (const juce::Component& window)
{
    for (auto* child : window.getChildren())
        if (child->getName() == "menu")
            return child;

    return nullptr;
}

// Whether a parameter's control shows in the editor.
bool shows (const phazite::ui::CompressorEditor& editor, const char* id)
{
    const auto* control = editor.findControl (id);
    return control != nullptr && control->isShowing();
}

// What each page shows of the compressor's parameters: compressing, its curve's knobs and its low cut (the high cut is Duck's
// alone, which compressing ignores); ducking, its own and both cuts.
const std::vector<const char*> compressKnobs { "attack", "release", "inputGain", "drive", "makeup", "keyLowCut" };
const std::vector<const char*> duckKnobs { "threshold", "depth", "attack", "hold", "release", "releaseCurve", "keyLowCut", "keyHighCut" };

// Clicks a mode's tab.
void clickMode (phazite::ui::CompressorEditor& editor, Compressor::Mode mode)
{
    auto& tab = editor.getModeTab ((int) mode);
    phazite::test::Mouse mouse (tab);
    mouse.click (tab.getLocalBounds().getCentre());
}
} // namespace

TEST_CASE ("The compressor's editor compresses at first: its characters, its curve, whose corner drags as one undo step, and no key")
{
    Song song;
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::CompressorEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::CompressorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // Its Compress tab in front: its characters, a row of buttons, and the knobs of what it compresses by; none of Duck's, no
    // key, and its mix its header's.
    CHECK (editor.getModeTab (0).getButtonText() == juce::String ("Compress"));
    CHECK (editor.getModeTab (1).getButtonText() == juce::String ("Duck"));
    CHECK (editor.getModeTab (0).getToggleState());
    CHECK_FALSE (editor.getModeTab (1).getToggleState());
    CHECK_FALSE (editor.isDucking());
    CHECK (shows (editor, "character"));

    for (const auto* id : compressKnobs)
    {
        CAPTURE (id);
        CHECK (shows (editor, id));
        CHECK (dynamic_cast<const phazite::ui::Knob*> (editor.findControl (id)) != nullptr);
    }

    CHECK_FALSE (shows (editor, "keyHighCut"));

    for (const auto* id : { "threshold", "depth", "hold", "releaseCurve" })
    {
        CAPTURE (id);
        CHECK_FALSE (shows (editor, id));
    }

    CHECK (editor.findControl ("mix") == nullptr);
    CHECK (editor.findControl ("points") == nullptr);
    CHECK (editor.getKeyPicker() == nullptr); // without the project
    CHECK_FALSE (editor.getListenButton().isShowing());
    phazite::test::checkChildrenApart (editor);

    // Its history beside the curve: what comes in and goes out, with no line to drag.
    CHECK (editor.getHistory().isShowing());
    CHECK (editor.getHistory().showsOutput());
    CHECK_FALSE (editor.getHistory().hasLine());

    // The curve whole, square, beside its meter, over its tools.
    auto& curve = editor.getCurve();
    CHECK (curve.isShowing());
    CHECK (curve.getWidth() == curve.getHeight());
    CHECK (curve.getWidth() >= 150);

    // Its corner at -18 dB dragged left, level (Shift): it turns down from lower.
    phazite::test::Mouse mouse (curve);
    const auto corner = curve.getPointPosition (1).roundToInt();
    mouse.press (corner, juce::ModifierKeys::shiftModifier);
    mouse.dragTo (corner - juce::Point<int> (20, 0));
    mouse.release();
    CHECK (song.valueOf ("point2X") < curves::axisForDecibels (-18.0f) - 0.02f);
    CHECK (song.valueOf ("point2Y") == doctest::Approx (curves::axisForDecibels (-18.0f)).epsilon (1.0e-4));
    CHECK (song.describeUndo() == juce::String ("Undo Move Point"));

    phazite::undo (*song.song);
    editor.refresh();
    CHECK (song.valueOf ("point2X") == doctest::Approx (curves::axisForDecibels (-18.0f)).epsilon (1.0e-4));
}

TEST_CASE ("The compressor's tabs are its modes: a click on one sets it, one undo step, and shows its page; an undo shows the other")
{
    Song song;
    phazite::ui::CompressorEditor editor (song.getDevice(), &song.context, song.compressor);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    CHECK (song.valueOf ("mode") == doctest::Approx ((float) Compressor::Mode::compress));
    REQUIRE (editor.getKeyPicker() != nullptr);
    CHECK_FALSE (editor.getKeyPicker()->isShowing());

    // Duck: the mode, one undo step, and its page: the key over its cuts and Listen, its knobs, and the history with the
    // threshold's line, the key in place of what comes in and goes out; no curve, no characters.
    clickMode (editor, Compressor::Mode::duck);
    CHECK (song.valueOf ("mode") == doctest::Approx ((float) Compressor::Mode::duck));
    CHECK (song.describeUndo() == juce::String ("Undo Change Mode"));
    CHECK (editor.isDucking());
    CHECK (editor.getModeTab (1).getToggleState());
    CHECK_FALSE (editor.getModeTab (0).getToggleState());
    CHECK (editor.getKeyPicker()->isShowing());
    CHECK (editor.getListenButton().isShowing());

    for (const auto* id : duckKnobs)
    {
        CAPTURE (id);
        CHECK (shows (editor, id));
    }

    for (const auto* id : { "character", "inputGain", "drive", "makeup" })
    {
        CAPTURE (id);
        CHECK_FALSE (shows (editor, id));
    }

    CHECK_FALSE (editor.getCurve().isShowing());
    CHECK (editor.getHistory().isShowing());
    CHECK (editor.getHistory().hasLine());
    CHECK_FALSE (editor.getHistory().showsOutput());
    phazite::test::checkChildrenApart (editor);

    // Compress again: the curve, and no key; then undone, at the tabs' next refresh, Duck's page again.
    clickMode (editor, Compressor::Mode::compress);
    CHECK (song.valueOf ("mode") == doctest::Approx ((float) Compressor::Mode::compress));
    CHECK (editor.getCurve().isShowing());
    CHECK_FALSE (editor.getKeyPicker()->isShowing());
    CHECK_FALSE (editor.getHistory().hasLine());
    phazite::undo (*song.song);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.isDucking());
    CHECK (editor.getModeTab (1).getToggleState());
    CHECK (editor.getKeyPicker()->isShowing());
}

TEST_CASE ("Ducking, the compressor's editor has its key's cuts, and Listen plays the key while Duck's page shows")
{
    Song song;
    phazite::ui::CompressorEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    clickMode (editor, Compressor::Mode::duck);
    CHECK (shows (editor, "keyLowCut"));
    CHECK (shows (editor, "keyHighCut"));

    // Off at first; on, the compressor plays its key, as long as the editor asks (each refresh).
    auto* compressor = song.getDevice().getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    editor.refresh();
    CHECK_FALSE (compressor->isPlayingKey());
    phazite::test::Mouse mouse (editor.getListenButton());
    mouse.click (editor.getListenButton().getLocalBounds().getCentre());
    CHECK (editor.getListenButton().getToggleState());
    editor.refresh();
    CHECK (compressor->isPlayingKey());

    // On Compress's page, where it hears its own sound, it no longer asks.
    juce::Thread::sleep (600);
    clickMode (editor, Compressor::Mode::compress);
    editor.refresh();
    CHECK_FALSE (compressor->isPlayingKey());
}

TEST_CASE ("The compressor's editor lights its curve up to the level it hears, and shows how far it turns the sound down or up")
{
    Song song;
    const auto device = song.getDevice();
    auto* compressor = device.getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    compressor->prepare (48000.0, 24000);

    // What played before the editor opened doesn't show.
    play (*compressor, -6.0f, 4800);
    phazite::ui::CompressorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();
    CHECK (editor.getShownReduction() == doctest::Approx (0.0f));
    CHECK (curve.getHeardX() == doctest::Approx (curve.getPosition (0.0f, 0.0f).x));

    // 12 dB over its corner, lit up to -6 dB and turned down as its curve says.
    play (*compressor, -6.0f, 24000);
    editor.refresh();
    CHECK (editor.getShownLevel() == doctest::Approx (-6.0f).epsilon (0.01));
    CHECK (curve.getHeardX() == doctest::Approx (curve.getPosition (curves::axisForDecibels (-6.0f), 0.0f).x).epsilon (0.01));
    CHECK (getDefaultReduction (-6.0f) > 8.0f);
    CHECK (editor.getShownReduction() == doctest::Approx (getDefaultReduction (-6.0f)).epsilon (0.01));
    CHECK (editor.getShownBoost() == doctest::Approx (0.0f));

    // Then, with nothing playing, the meters fall back smoothly.
    editor.refresh();
    CHECK (editor.getShownReduction() == doctest::Approx (getDefaultReduction (-6.0f) - 0.5f).epsilon (0.01));

    // Its curve lifting what is quiet: a quiet level comes out louder, and the meter says how far.
    REQUIRE (curve.applyShape ("Lift under -30 dB"));
    std::vector<float> values;

    for (const auto& parameter : Compressor::info.parameters)
        values.push_back (song.valueOf (parameter.id.data()));

    play (*compressor, -45.0f, 24000, values);
    editor.refresh();
    CHECK (editor.getShownBoost() > 3.0f);
}

TEST_CASE ("The compressor's characters are a row of buttons: a click picks one as one undo step, and the one in use is lit")
{
    Song song;
    phazite::ui::CompressorEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    editor.refresh();
    CHECK (editor.getCharacterButton (0).getToggleState()); // Clean at first
    CHECK (editor.getCharacterButton (0).getButtonText() == juce::String ("Clean"));
    CHECK (editor.getCharacterButton (4).getButtonText() == juce::String ("Tube"));

    // Opto: a click, one undo step, and its button lit alone.
    phazite::test::Mouse mouse (editor.getCharacterButton (3));
    mouse.click (editor.getCharacterButton (3).getLocalBounds().getCentre());
    CHECK (song.valueOf ("character") == doctest::Approx ((float) Compressor::Character::opto));
    CHECK (song.describeUndo() == juce::String ("Undo Change Character"));
    CHECK (editor.getCharacterButton (3).getToggleState());
    CHECK_FALSE (editor.getCharacterButton (0).getToggleState());

    // Undone, the editor shows Clean again at its next refresh.
    phazite::undo (*song.song);
    editor.refresh();
    CHECK (editor.getCharacterButton (0).getToggleState());
    CHECK_FALSE (editor.getCharacterButton (3).getToggleState());
}

TEST_CASE ("How far the compressor turns the sound down reads the same for any number of displays, then falls back")
{
    Song song;
    auto* compressor = song.getDevice().getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    compressor->prepare (48000.0, 24000);
    const auto* reduction = compressor->getGainReduction();
    REQUIRE (reduction != nullptr);
    CHECK (reduction->get() == doctest::Approx (0.0f));

    // 12 dB over its corner, turned down as its curve says; reading it takes nothing away from the next reader.
    play (*compressor, -6.0f, 24000);
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto first = reduction->get();
    CHECK (first == doctest::Approx (getDefaultReduction (-6.0f)).epsilon (0.02));
    CHECK (reduction->get() == doctest::Approx (first).epsilon (0.01));

    // With nothing played since (a device asleep on silence plays nothing), it falls 15 dB a second.
    juce::Thread::sleep (200);
    const auto seconds = (float) ((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0);
    CHECK (reduction->get() == doctest::Approx (first - 15.0f * seconds).epsilon (0.06));
}

TEST_CASE ("Ducking, the compressor's key is picked over its cuts and Listen: a track chosen there, one undo step, sets it up to duck")
{
    // The owner (2026-11-10): the button in the effect list "should be moved inside of the effect". As the user picks the key:
    // a click on Duck, then on the key, opens its menu (inside the window, as the app's menus open), the arrow keys go down to
    // the kick, and Return.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    const phazite::test::ScopedDisplay display; // JUCE places a menu on its display

    Song song;
    phazite::ui::CompressorEditor editor (song.getDevice(), &song.context, song.compressor);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    phazite::ui::setPopupsInside (window);
    clickMode (editor, Compressor::Mode::duck);

    // Above the key's cuts and Listen: none at first, the compressor hearing its own sound.
    auto* picker = editor.getKeyPicker();
    REQUIRE (picker != nullptr);
    CHECK (picker->isShowing());
    CHECK (picker->getTitle() == juce::String ("Key"));
    CHECK (picker->getButtonText() == juce::String ("None"));

    for (auto* below :
         { editor.findControl ("keyLowCut"), editor.findControl ("keyHighCut"), static_cast<juce::Component*> (&editor.getListenButton()) })
    {
        REQUIRE (below != nullptr);
        CHECK (picker->getScreenBounds().getBottom() <= below->getScreenBounds().getY());
    }

    // Its menu: None, ticked, then the other tracks, not the bass it is on.
    const auto menu = picker->getMenu();
    const auto* none = phazite::test::findMenuItem (menu, { "None (its own sound)" });
    REQUIRE (none != nullptr);
    CHECK (none->isTicked);
    CHECK (phazite::test::findMenuItem (menu, { "Kick" }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Keys" }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Bass" }) == nullptr);

    phazite::test::Mouse mouse (*picker);
    mouse.click (picker->getLocalBounds().getCentre());
    REQUIRE (findMenuWindow (window) != nullptr);
    window.pressKey (juce::KeyPress (juce::KeyPress::downKey));
    window.pressKey (juce::KeyPress (juce::KeyPress::downKey));
    window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    window.releaseKeys();
    phazite::test::dispatchPendingMessages();
    CHECK (findMenuWindow (window) == nullptr);

    // It listens to the kick, set up to duck as it was still as it was made but for its mode, all one undo step; the picker
    // and the history name the kick.
    CHECK (phazite::getSidechainSource (song.context, song.compressor) == song.getTrack (0));
    CHECK (song.describeUndo() == juce::String ("Undo Sidechain from Kick"));
    CHECK (song.valueOf ("threshold") == doctest::Approx (-14.0f));
    CHECK (song.valueOf ("hold") == doctest::Approx (120.0f));
    CHECK (song.valueOf ("release") == doctest::Approx (80.0f));
    CHECK (picker->getButtonText() == juce::String ("Kick"));
    editor.refresh();
    CHECK (editor.getHistory().getKeyName() == juce::String ("Kick"));

    // Compressing, the history shows its own sound, the kick kept for when it ducks again.
    clickMode (editor, Compressor::Mode::compress);
    CHECK (editor.getHistory().getKeyName().isEmpty());
    CHECK (phazite::getSidechainSource (song.context, song.compressor) == song.getTrack (0));
    clickMode (editor, Compressor::Mode::duck);
    CHECK (editor.getHistory().getKeyName() == juce::String ("Kick"));

    // Undone (both modes' clicks, then the key): none again, at the editor's next look.
    for (int step = 0; step < 3; ++step)
        phazite::undo (*song.song);

    editor.refresh();
    CHECK (picker->getButtonText() == juce::String ("None"));
    CHECK (editor.getHistory().getKeyName().isEmpty());
}

TEST_CASE ("Ducking, the threshold is a line across the history: near it an up-and-down arrow, lit, and a drag moves it, one undo step")
{
    Song song;
    phazite::ui::CompressorEditor editor (song.getDevice(), &song.context, song.compressor);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& history = editor.getHistory();
    CHECK_FALSE (history.hasLine()); // compressing, the curve sets the level
    clickMode (editor, Compressor::Mode::duck);
    REQUIRE (history.hasLine());
    CHECK (history.getDescription().contains ("Drag the threshold's line"));

    // On it: an up-and-down arrow, the line lit, and the hint saying what a drag does.
    phazite::test::Mouse mouse (history);
    const auto y = juce::roundToInt (history.getYForLevel (song.valueOf ("threshold")));
    mouse.moveTo ({ 60, history.getHeight() - 4 });
    CHECK_FALSE (history.isLineLit());
    mouse.moveTo ({ 60, y + 2 });
    CHECK (history.isLineLit());
    CHECK (history.getMouseCursor() == juce::MouseCursor::UpDownResizeCursor);
    CHECK (history.getTooltip().contains ("Drag up or down to move the threshold"));

    // Dragged up: the threshold rises, as far as the levels' scale says, one undo step.
    mouse.press ({ 60, y });
    mouse.dragTo ({ 60, y - 20 });
    mouse.release();
    const auto perPixel = 60.0f / (float) (history.getHeight() - 2);
    CHECK (song.valueOf ("threshold") == doctest::Approx (-24.0f + 20.0f * perPixel).epsilon (0.02));
    CHECK (song.describeUndo() == juce::String ("Undo Change Threshold"));
    phazite::undo (*song.song);
    CHECK (song.valueOf ("threshold") == doctest::Approx (-24.0f));

    // Back to compressing: no line, and nothing near where it was to light up.
    clickMode (editor, Compressor::Mode::compress);
    CHECK_FALSE (history.hasLine());
    mouse.moveTo ({ 60, y });
    CHECK_FALSE (history.isLineLit());
    CHECK (history.getMouseCursor() == juce::MouseCursor::NormalCursor);
}

TEST_CASE ("The compressor's history shows the last two bars at the song's tempo: 4 s at 120 BPM, 8 s at 60, two bars of 3/4")
{
    Song song;
    phazite::ui::CompressorEditor editor (song.getDevice(), &song.context, song.compressor);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& history = editor.getHistory();
    CHECK (history.isShowing());
    phazite::test::checkChildrenApart (editor);

    phazite::setTempo (*song.song, 120.0);
    editor.refresh();
    CHECK (history.getSpanMilliseconds() == doctest::Approx (4000.0));

    phazite::setTempo (*song.song, 60.0);
    editor.refresh();
    CHECK (history.getSpanMilliseconds() == doctest::Approx (8000.0));

    phazite::setTimeSignature (*song.song, { 3, 4 });
    editor.refresh();
    CHECK (history.getSpanMilliseconds() == doctest::Approx (6000.0));

    // Without a key, what it hears is its own sound.
    CHECK (history.getKeyName().isEmpty());
}

TEST_CASE ("In the default dock each of the compressor's modes shows whole on one page: no pages to turn")
{
    Song song;
    phazite::ui::CompressorEditor editor (song.getDevice(), &song.context, song.compressor);
    constexpr int dockWidth = 552, dockHeight = 268; // beside the Devices view's column
    REQUIRE (editor.getMinimumHeightAtWidth (dockWidth) <= dockHeight);
    phazite::test::TestWindow window (editor, dockWidth, dockHeight);

    // Compress: the characters, the curve with its meter and tools, and beside them the history over the knobs.
    CHECK_FALSE (editor.getLayout().usesPages());
    CHECK (editor.getModeTab (0).isShowing());
    CHECK (editor.getModeTab (1).isShowing());
    CHECK (editor.getCharacterButton (0).isShowing());
    CHECK (editor.getCurve().isShowing());
    CHECK (editor.getCurve().getWidth() >= 120);
    CHECK (editor.getHistory().isShowing());
    CHECK (editor.getHistory().getWidth() >= phazite::ui::GraphTint::minimumWidth);
    CHECK (editor.getHistory().getHeight() >= 96);

    for (const auto* id : compressKnobs)
    {
        CAPTURE (id);
        CHECK (shows (editor, id));
        CHECK (editor.findControl (id)->getWidth() >= phazite::ui::compactKnobSize);
    }

    phazite::test::checkChildrenApart (editor);

    // Duck: the key over its cuts and Listen, and beside them the history over the knobs, each a whole cell wide.
    clickMode (editor, Compressor::Mode::duck);
    CHECK_FALSE (editor.getLayout().usesPages());
    CHECK (editor.getKeyPicker()->isShowing());
    CHECK (editor.getListenButton().isShowing());
    CHECK (editor.getHistory().isShowing());
    CHECK (editor.getHistory().getWidth() >= 300);
    CHECK (editor.getHistory().getHeight() >= 96);

    for (const auto* id : duckKnobs)
    {
        CAPTURE (id);
        CHECK (shows (editor, id));
    }

    phazite::test::checkChildrenApart (editor);
}
