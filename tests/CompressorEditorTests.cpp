// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (src/ui/devices/CompressorEditor.h): its tabs, which are its modes, each its own page, a click on
// one an undo step; compressing, its characters' buttons, its knobs, its curve (the multiband's, src/ui/devices/DynamicsCurve.h)
// lit up to the level it hears, its meter both ways, and no key; ducking, its key picked over the key's cuts with Listen, its
// knobs, and its threshold a line to drag in its history; its history, two bars long at the song's tempo, on both pages; and
// each page whole in the default dock.
#include "GoldenFiles.h"
#include "PerformanceCounts.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
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

// What each page shows of the compressor's parameters: compressing, its curve's knobs and the cuts of what it hears;
// ducking, its own and both cuts; shaping transients, its punch, sustain, speed, makeup and low cut.
const std::vector<const char*> compressKnobs { "attack", "release", "levelHold", "lookahead", "inputGain", "drive", "makeup" };
const std::vector<const char*> duckKnobs { "threshold", "depth", "attack", "hold", "release", "releaseCurve", "keyLowCut", "keyHighCut" };
const std::vector<const char*> transientKnobs { "punch", "sustain", "speed", "makeup", "keyLowCut" };

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

    CHECK (shows (editor, "keyHighCut")); // the cuts of what it hears, its own sound or a key
    CHECK (shows (editor, "autoMakeup"));

    for (const auto* id : { "threshold", "depth", "hold", "releaseCurve" })
    {
        CAPTURE (id);
        CHECK_FALSE (shows (editor, id));
    }

    CHECK (editor.findControl ("mix") == nullptr);
    CHECK (editor.findControl ("points") == nullptr);
    CHECK (editor.getKeyPicker() == nullptr); // without the project
    CHECK (editor.getListenButton().isShowing());
    phazite::test::checkChildrenApart (editor);

    // Its history beside the curve: what comes in and goes out, and how far it turned down, with no line to drag.
    CHECK (editor.getHistory().isShowing());
    CHECK (editor.getHistory().showsReduction());
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
    CHECK (editor.getKeyPicker()->isShowing()); // keyed by the track it picks

    // Duck: the mode, one undo step, and its page: the key over its cuts and Listen, its knobs, and the history with the
    // threshold's line; no curve, no characters.
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
    phazite::test::checkChildrenApart (editor);

    // Compress again: the curve, and the key it can hear; then undone, at the tabs' next refresh, Duck's page again.
    clickMode (editor, Compressor::Mode::compress);
    CHECK (song.valueOf ("mode") == doctest::Approx ((float) Compressor::Mode::compress));
    CHECK (editor.getCurve().isShowing());
    CHECK (editor.getKeyPicker()->isShowing());
    CHECK_FALSE (editor.getHistory().hasLine());
    phazite::undo (*song.song);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.isDucking());
    CHECK (editor.getModeTab (1).getToggleState());
    CHECK (editor.getKeyPicker()->isShowing());
}

TEST_CASE ("The compressor's editor has its key's cuts, and Listen plays the key while Compress's or Duck's page shows")
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
    phazite::test::HeldClock clock;
    editor.refresh();
    CHECK_FALSE (compressor->isPlayingKey());
    phazite::test::Mouse mouse (editor.getListenButton());
    mouse.click (editor.getListenButton().getLocalBounds().getCentre());
    CHECK (editor.getListenButton().getToggleState());
    editor.refresh();
    CHECK (compressor->isPlayingKey());

    // On Compress's page, which shows the same Listen, it asks on; on Transient's, which hears its own sound and has no
    // Listen, it no longer asks: once the last ask has run out, the key stops.
    clock.advance (501.0);
    clickMode (editor, Compressor::Mode::compress);
    editor.refresh();
    CHECK (compressor->isPlayingKey());
    clock.advance (501.0);
    clickMode (editor, Compressor::Mode::transient);
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

TEST_CASE ("Compressing, the history draws what came in as it came in, not as the detector hears it after the input gain")
{
    // Known bug 119: the history drew the detector's level as what came in.
    Song song;
    const auto device = song.getDevice();
    auto* compressor = device.getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    compressor->prepare (48000.0, 24000);
    phazite::ui::CompressorEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    editor.refresh(); // it listens from now on
    auto* scope = device.getScope();
    REQUIRE (scope != nullptr);
    REQUIRE (scope->isListening());

    // -12 dB comes in, which the input gain turns up 12 dB before the detector hears it.
    auto values = getDefaults();
    auto found = false;

    for (size_t i = 0; i < values.size(); ++i)
    {
        if (Compressor::info.parameters[i].id == "inputGain")
        {
            values[i] = 12.0f;
            found = true;
        }
    }

    REQUIRE (found);
    std::vector<float> in (4800, juce::Decibels::decibelsToGain (-12.0f));
    scope->pushInput (in.data(), in.data(), (int) in.size());
    play (*compressor, -12.0f, 24000, values);
    editor.refresh();
    CHECK (editor.getShownLevel() > -3.0f); // what the detector heard
    CHECK (editor.getHistory().getNewestInput() == doctest::Approx (-12.0f).epsilon (0.01));
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

    // 12 dB over its corner, turned down as its curve says; reading it takes nothing away from the next reader. It falls by
    // the clock from its last block on, so each reading is checked against the time since before that block, measured
    // around it: however long the machine kept this thread from running (to within 2 % alone, it failed beside other
    // builds: known bug 178's survey).
    const auto now = [] { return juce::Time::getMillisecondCounterHiRes() / 1000.0; };
    const auto expected = getDefaultReduction (-6.0f);
    const auto beforePlayed = now();
    play (*compressor, -6.0f, 24000);
    const auto beforeFirst = now();
    const auto first = reduction->get();
    const auto afterFirst = now();
    CHECK (first <= expected * 1.02f);
    CHECK (first >= expected * 0.98f - 15.0f * (float) (afterFirst - beforePlayed));
    const auto again = reduction->get();
    const auto afterAgain = now();
    CHECK (again <= first * 1.01f);
    CHECK (again >= first * 0.99f - 15.0f * (float) (afterAgain - beforeFirst));

    // With nothing played since (a device asleep on silence plays nothing), it falls 15 dB a second, by the clock: as far
    // as the time between the two readings, measured around each of them (however long the machine kept this thread from
    // running), and no lower than none.
    juce::Thread::sleep (200); // a lower bound only: at least so long has passed, however long it took
    const auto beforeSecond = now();
    const auto second = reduction->get();
    const auto afterSecond = now();
    const auto fell = [first] (double seconds) { return std::max (0.0f, first - 15.0f * (float) seconds); };
    CHECK (second <= fell (beforeSecond - afterFirst) + 0.05f);
    CHECK (second >= fell (afterSecond - beforeFirst) - 0.05f);
    CHECK (second < first - 2.5f); // 200 ms at least: 3 dB
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

    // Above the sidechain's cuts and Listen: none at first, the compressor hearing its own sound.
    auto* picker = editor.getKeyPicker();
    REQUIRE (picker != nullptr);
    CHECK (picker->isShowing());
    CHECK (picker->getTitle() == juce::String ("Sidechain"));
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

    // A click opens it. The kick is then chosen as its item runs: keys reach a menu differently in each system's test window
    // (in CI's sanitizer builds on Linux the menu hadn't taken them yet).
    phazite::test::Mouse mouse (*picker);
    mouse.click (picker->getLocalBounds().getCentre());
    REQUIRE (findMenuWindow (window) != nullptr);
    juce::PopupMenu::dismissAllActiveMenus();
    phazite::test::dispatchPendingMessages();
    CHECK (findMenuWindow (window) == nullptr);
    REQUIRE (phazite::test::runMenuItem (picker->getMenu(), { "Kick" }));
    phazite::test::dispatchPendingMessages();

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
    CHECK_FALSE (editor.getHistory().showsReduction()); // what the kick does is the bass's dips
    CHECK (editor.getHistory().getDescription().contains ("Kick, what it listens to"));

    // Compressing, keyed by the kick, the history shows it too; shaping transients, its own sound, the kick kept for when it
    // compresses or ducks again.
    clickMode (editor, Compressor::Mode::compress);
    CHECK (editor.getHistory().getKeyName() == juce::String ("Kick"));
    clickMode (editor, Compressor::Mode::transient);
    CHECK (editor.getHistory().getKeyName().isEmpty());
    CHECK (phazite::getSidechainSource (song.context, song.compressor) == song.getTrack (0));
    clickMode (editor, Compressor::Mode::duck);
    CHECK (editor.getHistory().getKeyName() == juce::String ("Kick"));

    // Undone (the modes' clicks, then the key): none again, at the editor's next look.
    for (int step = 0; step < 4; ++step)
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

    // Compress: the characters, the curve with its meter, its tools and Auto Makeup; and beside them its knobs, under a tab
    // beside the history's and the key's, which the dock is too short to show over one another.
    CHECK_FALSE (editor.getLayout().usesPages());
    CHECK (editor.getModeTab (0).isShowing());
    CHECK (editor.getModeTab (1).isShowing());
    CHECK (editor.getModeTab (2).isShowing());
    CHECK (editor.getCharacterButton (0).isShowing());
    CHECK (editor.getCurve().isShowing());
    CHECK (editor.getCurve().getWidth() >= 120);
    CHECK (shows (editor, "autoMakeup"));
    REQUIRE (editor.getLayout().usesTabs (1));
    CHECK (editor.getLayout().getShownPart (1) == 1);

    for (const auto* id : compressKnobs)
    {
        CAPTURE (id);
        CHECK (shows (editor, id));
        CHECK (editor.findControl (id)->getWidth() >= phazite::ui::compactKnobSize);
    }

    phazite::test::checkChildrenApart (editor);

    // Its history and its key, each under its tab.
    editor.getLayout().showPart (1, 0);
    CHECK (editor.getHistory().isShowing());
    CHECK (editor.getHistory().getWidth() >= phazite::ui::GraphTint::minimumWidth);
    CHECK (editor.getHistory().getHeight() >= 96);
    editor.getLayout().showPart (1, 2);
    CHECK (editor.getKeyPicker()->isShowing());
    CHECK (editor.getListenButton().isShowing());
    CHECK (shows (editor, "keyLowCut"));
    CHECK (shows (editor, "keyHighCut"));
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

    // Transient: the history beside its punch and sustain, large, over its speed, makeup and low cut.
    clickMode (editor, Compressor::Mode::transient);
    CHECK_FALSE (editor.getLayout().usesPages());
    CHECK (editor.isShapingTransients());
    CHECK (editor.getHistory().isShowing());
    CHECK (editor.getHistory().getWidth() >= 300);
    CHECK (editor.getHistory().getHeight() >= 96);
    CHECK_FALSE (editor.getCurve().isShowing());
    CHECK_FALSE (editor.getListenButton().isShowing());

    for (const auto* id : transientKnobs)
    {
        CAPTURE (id);
        CHECK (shows (editor, id));
        CHECK (editor.findControl (id)->getWidth() >= phazite::ui::compactKnobSize);
    }

    CHECK (editor.findControl ("punch")->getWidth() >= phazite::ui::knobSize); // large
    CHECK (editor.findControl ("sustain")->getWidth() >= phazite::ui::knobSize);
    phazite::test::checkChildrenApart (editor);
}

TEST_CASE ("Shaping transients, the compressor's history shows its gain up and down about its middle, and no key")
{
    Song song;
    phazite::setSidechainSource (song.context, song.compressor, song.getTrack (0));
    song.song->getUndoManager().clearUndoHistory();
    phazite::ui::CompressorEditor editor (song.getDevice(), &song.context, song.compressor);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& history = editor.getHistory();

    // Compressing, keyed by the kick: the kick as a line in its colour, named, and how far it turned down from the top no
    // more.
    phazite::test::dispatchPendingMessages();
    editor.refresh();
    REQUIRE (Compressor::getMode (song.valueOf ("mode")) == Compressor::Mode::duck); // set up to duck as it was made
    clickMode (editor, Compressor::Mode::compress);
    editor.refresh();
    CHECK (history.getKeyName() == juce::String ("Kick"));
    CHECK_FALSE (history.showsReduction());
    CHECK_FALSE (history.showsGainAroundMiddle());

    // Transient: no key, its gain about the middle; a click on its tab one undo step.
    clickMode (editor, Compressor::Mode::transient);
    CHECK (song.valueOf ("mode") == doctest::Approx ((float) Compressor::Mode::transient));
    CHECK (song.describeUndo() == juce::String ("Undo Change Mode"));
    editor.refresh();
    CHECK (history.getKeyName().isEmpty());
    CHECK (history.showsGainAroundMiddle());

    // Turning a hit up: the newest reading is below 0, how far it turned up.
    auto* compressor = song.getDevice().getProcessor<Compressor>();
    REQUIRE (compressor != nullptr);
    compressor->prepare (48000.0, 4800);
    auto values = getDefaults();
    values[Compressor::mode] = (float) Compressor::Mode::transient;
    values[Compressor::punch] = 1.0f;
    juce::AudioBuffer<float> buffer (2, 4800);
    buffer.clear();

    for (int i = 2400; i < 4800; ++i)
        for (int channel = 0; channel < 2; ++channel)
            buffer.setSample (channel, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 200.0 * i / 48000.0));

    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = 4800;
    block.parameters = values;
    compressor->process (block);
    editor.refresh();
    CHECK (history.getNewestReduction() < -10.0f);
    CHECK (editor.getShownBoost() > 10.0f);
}

TEST_CASE ("The compressor's curve by numbers: its corner reads a point and its line's ratio, typed levels and the arrows move it")
{
    Song song;
    phazite::ui::CompressorEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();
    const auto decibelsOf = [&song] (const char* id) { return juce::Decibels::gainToDecibels (curves::fromAxis (song.valueOf (id))); };

    // The default curve's corner at -18 dB, as numbers, and the line that leaves it, 4:1 to 0 dB; from silence, no ratio.
    CHECK (curve.describePoint (1) == juce::String ("In -18.0 dB, Out -18.0 dB"));
    CHECK (curve.describeRatio (1) == juce::String ("4.0:1"));
    CHECK (curve.describeRatio (0).isEmpty());
    CHECK (curve.describeRatio (curve.getCount() - 1).isEmpty()); // the last point: no line leaves it

    // Under the pointer, the corner reads it.
    CHECK (curve.getReadout().isEmpty());
    phazite::test::Mouse mouse (curve);
    mouse.moveTo (curve.getPointPosition (1).roundToInt());
    CHECK (curve.getReadout() == juce::String ("In -18.0 dB, Out -18.0 dB\n4.0:1"));

    // Ratios: a line that expands, a limit, a gate; and one that lifts quiet levels, 2:1 as it rises.
    REQUIRE (curve.applyShape ("Lift under -30 dB")); // from -60 dB in, -45 out, to -30 dB both
    editor.refresh();
    CHECK (curve.describeRatio (1) == juce::String ("2.0:1"));
    REQUIRE (curve.applyShape ("Expand under -24 dB")); // from -40 dB in, -56 out, to -24 dB both
    editor.refresh();
    CHECK (curve.describeRatio (1) == juce::String ("1:2.0"));
    REQUIRE (curve.applyShape ("Limit at 0 dB"));
    editor.refresh();
    CHECK (curve.describeRatio (1) == juce::String ("Limit"));
    REQUIRE (curve.applyShape ("Gate under -40 dB"));
    editor.refresh();
    CHECK (curve.describeRatio (1) == juce::String ("Gate"));
    REQUIRE (curve.applyShape ("Compress 4:1 over -18 dB"));
    editor.refresh();

    // Type Values...: "in, out" in decibels, exactly, one undo step named as a drag's; the curve's end its level out alone.
    REQUIRE (curve.setPointLevels (1, "-24, -20 dB"));
    CHECK (decibelsOf ("point2X") == doctest::Approx (-24.0f).epsilon (1.0e-4));
    CHECK (decibelsOf ("point2Y") == doctest::Approx (-20.0f).epsilon (1.0e-4));
    CHECK (song.describeUndo() == juce::String ("Undo Move Point"));
    phazite::undo (*song.song);
    editor.refresh();
    CHECK (decibelsOf ("point2X") == doctest::Approx (-18.0f).epsilon (1.0e-4));
    CHECK (decibelsOf ("point2Y") == doctest::Approx (-18.0f).epsilon (1.0e-4));
    const auto last = curve.getCount() - 1;
    REQUIRE (curve.setPointLevels (last, "-3"));
    CHECK (song.valueOf ("point4X") == doctest::Approx (1.0f));
    CHECK (decibelsOf ("point4Y") == doctest::Approx (-3.0f).epsilon (1.0e-4));
    CHECK_FALSE (curve.setPointLevels (1, "loud"));
    CHECK_FALSE (curve.setPointLevels (1, "-12")); // a point between needs both
    phazite::undo (*song.song);
    editor.refresh();

    // From its menu, the field over the point, with its levels: Enter moves it.
    curve.typeValues (1);
    auto* field = curve.getValuesField();
    REQUIRE (field != nullptr);
    CHECK (field->getText() == juce::String ("-18.0, -18.0"));
    field->setText ("-30, -28");
    field->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (decibelsOf ("point2X") == doctest::Approx (-30.0f).epsilon (1.0e-4));
    CHECK (decibelsOf ("point2Y") == doctest::Approx (-28.0f).epsilon (1.0e-4));
    phazite::undo (*song.song);
    editor.refresh();

    // The keyboard: Tab reaches it; Right chooses the next point, and with Ctrl the arrows move it a tenth of a decibel, with
    // Shift a decibel; a run of presses is one undo step, which the next choice ends.
    CHECK (curve.getWantsKeyboardFocus());
    CHECK (curve.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (curve.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (curve.getChosenPoint() == 0);
    CHECK (curve.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (curve.getChosenPoint() == 1);
    const auto before = song.describeUndo();
    curve.keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0));
    CHECK (decibelsOf ("point2X") == doctest::Approx (-17.9f).epsilon (1.0e-3));
    curve.keyPressed (juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0));
    curve.keyPressed (juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0));
    CHECK (decibelsOf ("point2Y") == doctest::Approx (-16.0f).epsilon (1.0e-3));
    CHECK (decibelsOf ("point2X") == doctest::Approx (-17.9f).epsilon (1.0e-3));
    CHECK (curve.keyPressed (juce::KeyPress (juce::KeyPress::leftKey))); // chooses the first point: the run ends
    CHECK (curve.getChosenPoint() == 0);
    CHECK (song.describeUndo() == juce::String ("Undo Move Point"));
    CHECK (song.describeUndo() != before);
    phazite::undo (*song.song);
    editor.refresh();
    CHECK (decibelsOf ("point2X") == doctest::Approx (-18.0f).epsilon (1.0e-4));
    CHECK (decibelsOf ("point2Y") == doctest::Approx (-18.0f).epsilon (1.0e-4));
    CHECK (song.describeUndo() == before);
}

TEST_CASE ("The compressor's curve by keys, as a user presses them: Tab reaches it, and a key not its own ends a run of presses")
{
    Song song;
    phazite::ui::CompressorEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();

    // Tab from the characters' first button, through the window's focus traversal, reaches the curve.
    editor.getCharacterButton (0).grabKeyboardFocus();
    REQUIRE (editor.getCharacterButton (0).hasKeyboardFocus (false));

    for (int i = 0; i < 40 && ! curve.hasKeyboardFocus (false); ++i)
        window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));

    REQUIRE (curve.hasKeyboardFocus (false));

    // Right chooses the corner, Ctrl and Up moves it; a key that isn't the curve's ends the run, a step of its own, there and
    // then (the next key's action, Delete say, landed in it).
    window.pressKey (juce::KeyPress (juce::KeyPress::rightKey));
    REQUIRE (curve.getChosenPoint() == 1);
    window.pressKey (juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::commandModifier, 0));
    CHECK (phazite::Gesture::isInProgress (*song.song));
    window.pressKey (juce::KeyPress ('a'));
    CHECK_FALSE (phazite::Gesture::isInProgress (*song.song));
    CHECK (song.describeUndo() == juce::String ("Undo Move Point"));
    CHECK (juce::Decibels::gainToDecibels (curves::fromAxis (song.valueOf ("point2Y"))) == doctest::Approx (-17.9f).epsilon (1.0e-3));
}

TEST_CASE ("Type Values reads a decimal comma where it can't be mistaken, and says why it refuses what it can't read")
{
    Song song;
    phazite::ui::CompressorEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& curve = editor.getCurve();
    const auto decibelsOf = [&song] (const char* id) { return juce::Decibels::gainToDecibels (curves::fromAxis (song.valueOf (id))); };
    const auto last = curve.getCount() - 1;

    // An end's "-3,5" is -3.5 dB; a point between's "-18,5, -12" is -18.5 in and -12 out; "-20;-14" and "-20 -14" alike.
    REQUIRE (curve.setPointLevels (last, "-3,5"));
    CHECK (decibelsOf ("point4Y") == doctest::Approx (-3.5f).epsilon (1.0e-4));
    REQUIRE (curve.setPointLevels (1, "-18,5, -12"));
    CHECK (decibelsOf ("point2X") == doctest::Approx (-18.5f).epsilon (1.0e-4));
    CHECK (decibelsOf ("point2Y") == doctest::Approx (-12.0f).epsilon (1.0e-4));
    REQUIRE (curve.setPointLevels (1, "-20;-14"));
    CHECK (decibelsOf ("point2Y") == doctest::Approx (-14.0f).epsilon (1.0e-4));
    REQUIRE (curve.setPointLevels (1, "-21 -15 dB"));
    CHECK (decibelsOf ("point2X") == doctest::Approx (-21.0f).epsilon (1.0e-4));
    CHECK (curve.getRefusal().isEmpty());

    // What it can't read: refused, the point where it was, and why, in the curve's corner.
    CHECK_FALSE (curve.setPointLevels (1, "-12"));
    CHECK (curve.getRefusal().contains ("in and out"));
    CHECK (curve.getReadout() == curve.getRefusal());
    CHECK_FALSE (curve.setPointLevels (1, "loud, -12"));
    CHECK (curve.getRefusal().contains ("\"loud\" isn't a level"));
    CHECK_FALSE (curve.setPointLevels (last, "-3, -4"));
    CHECK (curve.getRefusal().contains ("level out alone"));

    // A number alone, its sign, a decimal point or comma, and "dB": "1e3" isn't +13 dB, nor "abc5" 5 dB.
    for (const auto* text : { "1e3, -12", "abc5, -12", "-18, 5x", "--18, -12", "-1.8.5, -12" })
    {
        CAPTURE (text);
        CHECK_FALSE (curve.setPointLevels (1, text));
        CHECK (curve.getRefusal().contains ("isn't a level"));
    }

    // The refusal shows until the next edit or move of the pointer: a key press is one.
    curve.keyPressed (juce::KeyPress ('a'));
    CHECK (curve.getRefusal().isEmpty());
    CHECK (curve.getReadout().isEmpty());
    CHECK (decibelsOf ("point2X") == doctest::Approx (-21.0f).epsilon (1.0e-4));
    REQUIRE (curve.setPointLevels (1, "-18, -18"));
    CHECK (curve.getRefusal().isEmpty());
}

TEST_CASE ("On Compress's page the Attack and the Release say what they do to a gate, which closes at one and opens at the other")
{
    Song song;
    phazite::ui::CompressorEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    const auto tooltipOf = [&editor] (const char* id)
    {
        auto* client = dynamic_cast<juce::SettableTooltipClient*> (editor.findControl (id));
        REQUIRE (client != nullptr);
        return client->getTooltip();
    };

    CHECK (tooltipOf ("attack").contains ("a gate closing"));
    CHECK (tooltipOf ("release").contains ("a gate opening"));
    const auto menu = editor.getCurve().isShowing() ? phazite::ui::DynamicsCurveTools (editor.getCurve()).getMenu() : juce::PopupMenu();
    CHECK (phazite::test::findMenuItem (menu, { "Shapes", "Gates: the Attack closes them, the Release opens them" }) != nullptr);
}

TEST_CASE ("Five notches of the wheel on the Look-Ahead are one undo step and one new plan, after the burst ends (known bug 201)")
{
    // The Look-Ahead's latency is the plan's: a plan at each notch remade the compressor at each, a gap in the sound each time.
    Song song;
    phazite::ui::CompressorEditor editor ((song.getDevice()));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto* knob = dynamic_cast<phazite::ui::Knob*> (editor.findControl ("lookahead"));
    REQUIRE (knob != nullptr);
    REQUIRE (knob->isShowing());

    // A value set by hand already: the first adds the device's values, a plan of its own (PerformanceGateTests.cpp).
    song.getDevice().getParameter ("attack").setValue (20.0f);
    song.song->getUndoManager().clearUndoHistory();
    phazite::test::PlanCounter plans (song.context);
    plans.take();
    phazite::test::UndoCheck undo (*song.song);
    const auto before = song.valueOf ("lookahead");

    undo.check ("five notches of the wheel",
                [&]
                {
                    phazite::test::Mouse mouse (*knob);
                    auto last = before;

                    // The value and its text follow each notch at once, and no plan comes while the burst goes on.
                    for (int notch = 0; notch < 5; ++notch)
                    {
                        CAPTURE (notch);
                        mouse.wheel (knob->getLocalBounds().getCentre(), (float) phazite::ui::wheelNotch);
                        const auto now = song.valueOf ("lookahead");
                        CHECK (now > last);
                        CHECK (knob->getValue() == doctest::Approx (now));
                        CHECK (knob->getTextFromValue (knob->getValue()) == knob->getTextFromValue (now));
                        CHECK (plans.take() == 0);
                        last = now;
                    }

                    phazite::ui::WheelBurst::endAll();
                    CHECK_FALSE (phazite::Gesture::isInProgress (*song.song));
                    CHECK (plans.take() == 1);
                });

    CHECK (undo.getNumSteps() == 1);
    CHECK (song.describeUndo() == juce::String ("Undo Change Look-Ahead"));
    phazite::undo (*song.song);
    CHECK (song.valueOf ("lookahead") == doctest::Approx (before));
}
