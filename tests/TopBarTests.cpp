// SPDX-License-Identifier: AGPL-3.0-or-later
// The top bar (src/ui/shell/TopBar.h), driven with the mouse: its tempo field and time signature show the open
// project's, and change them.
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/EngineContext.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/shell/TopBar.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
// A top bar in a window, over a project with one track. A window taller than the bar has room for what shows under it.
struct TopBarScreen
{
    explicit TopBarScreen (int height = 40)
        : windowHeight (height)
    {
        setSong (context, song.get());
        phazite::addMetronomeCommands (registry, getSelection (context), phazite::getAudioIO (phazite::test::getEngineContext()));
        song->getUndoManager().clearUndoHistory();

        // The bar shows the project at its next update.
        REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return getTempoField().isEnabled(); }));
    }

    ~TopBarScreen() { setSong (context, nullptr); }

    phazite::ui::NumberField& getTempoField()
    {
        auto* field = dynamic_cast<phazite::ui::NumberField*> (phazite::test::findComponentWithTitle (topBar, "Tempo"));
        REQUIRE (field != nullptr);
        return *field;
    }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge bridge { registry };
    phazite::ui::ComputerKeyboard keyboard { [] (const juce::MidiMessage&) {} };
    phazite::ui::TopBar topBar { bridge, keyboard, context };
    const int windowHeight;
    phazite::test::TestWindow window { topBar, 1200, windowHeight };
};
} // namespace

TEST_CASE ("While the tempo changes, the top bar shows what it stretches, with a switch for samples and one for patterns")
{
    TopBarScreen screen (300);
    phazite::addTempoCommands (screen.registry, getSelection (screen.context));

    // A loop that follows the tempo, a sound that doesn't, and a pattern on a track of its own.
    phazite::test::TemporaryFolder temp;
    auto& song = *screen.song;
    const auto track = phazite::getTrackIDs (song).front();
    REQUIRE (phazite::importFile (song, track, phazite::test::writeSound (temp.folder.getChildFile ("Loop 120 BPM.wav"), 4.0),
                                  phazite::BeatPosition())
                 .isValid());
    REQUIRE (phazite::importFile (song, track, phazite::test::writeSound (temp.folder.getChildFile ("One-second hit.wav"), 1.0),
                                  phazite::BeatPosition::fromBeats (10.0))
                 .isValid());
    const auto other = phazite::insertTrack (song, track, nullptr);
    REQUIRE (other.isValid());
    REQUIRE (phazite::insertMidiClip (song, other, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) }, nullptr).isValid());
    screen.song->getUndoManager().clearUndoHistory();

    // Nothing shows until the tempo is dragged.
    auto* window = screen.topBar.getTopLevelComponent();
    REQUIRE (window != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*window, "Changing the tempo stretches") == nullptr);

    auto& field = screen.getTempoField();
    phazite::test::Mouse mouse (field);
    const auto centre = field.getLocalBounds().getCentre();
    mouse.press (centre);
    mouse.dragTo (centre.translated (0, -4));
    mouse.release();

    auto* panel = phazite::test::findComponentWithTitle (*window, "Changing the tempo stretches");
    REQUIRE (panel != nullptr);
    CHECK (panel->isShowing());
    CHECK (panel->getY() >= screen.topBar.getLocalArea (&field, field.getLocalBounds()).getBottom()); // under the field

    auto* samples = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "Samples follow the tempo"));
    auto* patterns = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "Patterns follow the tempo"));
    REQUIRE (samples != nullptr);
    REQUIRE (patterns != nullptr);
    CHECK_FALSE (samples->getToggleState());
    CHECK (samples->getDescription() == juce::String ("1 of 2 follow the tempo"));
    CHECK (patterns->getToggleState());
    CHECK (patterns->getDescription() == juce::String ("It follows the tempo"));

    // Samples: now all of them follow it, as one undo step.
    phazite::test::Mouse samplesMouse (*samples);
    samplesMouse.click (samples->getLocalBounds().getCentre());
    CHECK (samples->getToggleState());
    CHECK (samples->getDescription() == juce::String ("All 2 follow the tempo"));
    CHECK (screen.song->getUndoManager().getUndoDescription() == juce::String ("Samples Follow Tempo"));

    // Patterns: now they keep their speed.
    phazite::test::Mouse patternsMouse (*patterns);
    patternsMouse.click (patterns->getLocalBounds().getCentre());
    CHECK_FALSE (patterns->getToggleState());
    CHECK (patterns->getDescription() == juce::String ("It keeps its speed"));
    CHECK_FALSE (phazite::doPatternsFollowTempo (*screen.song));

    // A press anywhere else hides it at once, as a popup goes.
    phazite::test::Mouse barMouse (screen.topBar);
    const juce::Point<int> elsewhere (screen.topBar.getWidth() / 2, 2);
    barMouse.click (elsewhere);
    CHECK_FALSE (panel->isShowing());

    // A click on the field, to type a tempo, shows it too; a press elsewhere hides it although the field keeps the keys.
    mouse.click (centre);
    phazite::test::dispatchPendingMessages();
    CHECK (panel->isShowing());
    barMouse.click (elsewhere);
    CHECK_FALSE (panel->isShowing());
}

TEST_CASE ("The top bar's tempo field shows the song's tempo, and a drag on it is one undo step")
{
    TopBarScreen screen;
    auto& field = screen.getTempoField();
    CHECK (field.getValue() == doctest::Approx (120.0));
    CHECK (field.getTextFromValue (field.getValue()) == juce::String ("120 BPM"));

    // Dragged up 20 pixels, a step every 2, over several moves.
    const auto centre = field.getLocalBounds().getCentre();
    phazite::test::Mouse mouse (field);
    mouse.press (centre);
    mouse.dragTo (centre.translated (0, -10));
    mouse.dragTo (centre.translated (0, -20));
    mouse.release();
    CHECK (phazite::getTempo (*screen.song) == doctest::Approx (130.0));
    CHECK (screen.song->getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));

    phazite::undo (*screen.song);
    CHECK (phazite::getTempo (*screen.song) == doctest::Approx (120.0)); // the whole drag
    CHECK_FALSE (screen.song->getUndoManager().canUndo());

    // A change made elsewhere shows there.
    phazite::setTempo (*screen.song, 95.5);
    CHECK (phazite::test::dispatchMessagesUntil ([&field] { return juce::approximatelyEqual (field.getValue(), 95.5); }));
}

TEST_CASE ("Escape in the middle of a drag of the tempo, the playhead's position or its time puts back what was there")
{
    // The fields put their own value back, while the tempo stayed where it was dragged, its step kept, and the playhead where
    // the drag took it (a reviewer, 2026-11-14).
    TopBarScreen screen;
    auto& tempo = screen.getTempoField();
    const auto escape = juce::KeyPress (juce::KeyPress::escapeKey);

    const auto dragThenEscape = [&screen, &escape] (juce::Component& field, const std::function<bool()>& moved)
    {
        const auto centre = field.getLocalBounds().getCentre();
        phazite::test::Mouse mouse (field);
        mouse.press (centre);
        mouse.dragTo (centre.translated (0, -10));
        mouse.dragTo (centre.translated (0, -20));
        CHECK (moved());
        screen.window.pressKey (escape);
        mouse.dragTo (centre.translated (0, -40)); // nothing more until the mouse goes up
        mouse.release();
        mouse.pause();
    };

    dragThenEscape (tempo, [&screen] { return phazite::getTempo (*screen.song) > 121.0; });
    CHECK (phazite::getTempo (*screen.song) == doctest::Approx (120.0));
    CHECK (tempo.getValue() == doctest::Approx (120.0));
    CHECK_FALSE (screen.song->getUndoManager().canUndo());
    CHECK_FALSE (screen.song->getUndoManager().canRedo());

    phazite::movePlayhead (*screen.song, phazite::BeatPosition::fromBeats (8.0));
    phazite::test::dispatchPendingMessages();

    for (const auto* title : { "Playhead position", "Playhead time" })
    {
        auto* field = phazite::test::findComponentWithTitle (screen.topBar, title);
        REQUIRE (field != nullptr);
        dragThenEscape (*field, [&screen] { return ! juce::approximatelyEqual (phazite::getPlayheadBeat (*screen.song).inBeats(), 8.0); });
        CHECK (phazite::getPlayheadBeat (*screen.song).inBeats() == doctest::Approx (8.0));
        CHECK (phazite::getPlayStartBeat (*screen.song).inBeats() == doctest::Approx (8.0));
    }
}

TEST_CASE ("The top bar's controls are drawn along its top, all as tall, the tempo field's box too")
{
    // The owner, 2026-11-07: the tempo field looked shorter than the buttons beside it (JUCE kept a pixel around a bar slider's
    // box for a border that Phazite doesn't draw), and the bar had more room above its controls than below them.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    TopBarScreen screen (phazite::ui::Shell::transportBarHeight);
    auto* play = phazite::test::findComponentWithTitle (screen.topBar, "Play");
    auto* timeSignature = phazite::test::findComponentWithTitle (screen.topBar, "Time signature");
    REQUIRE (play != nullptr);
    REQUIRE (timeSignature != nullptr);

    // Each one's outline: the first and the last rows of its middle column drawn in anything but the bar's colour. The bar
    // fills the window.
    const auto image = screen.window.paintEverything();
    const auto bar = phazite::ui::getPalette (screen.topBar).surface;

    for (auto* control : { play, static_cast<juce::Component*> (&screen.getTempoField()), timeSignature })
    {
        CAPTURE (control->getTitle());
        const auto x = screen.topBar.getLocalArea (control, control->getLocalBounds()).getCentreX();
        auto first = -1, last = -1;

        for (int y = 0; y < image.getHeight(); ++y)
        {
            if (image.getPixelAt (x, y) != bar)
            {
                first = first < 0 ? y : first;
                last = y;
            }
        }

        CHECK (first == 0);
        CHECK (last == phazite::ui::controlHeight - 1);
    }
}

TEST_CASE ("The top bar shows the song's time signature")
{
    TopBarScreen screen;
    auto* button = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (screen.topBar, "Time signature"));
    REQUIRE (button != nullptr);
    CHECK (button->getButtonText() == juce::String ("4/4"));

    phazite::setTimeSignature (*screen.song, { 6, 8 });
    CHECK (phazite::test::dispatchMessagesUntil ([button] { return button->getButtonText() == "6/8"; }));
}

TEST_CASE ("The top bar shows where the playhead is, in bars and in time, and a place typed in either takes the playhead there")
{
    // The owner's UX audit, 2026-11-12: the playhead's place showed nowhere, where every DAW checked has a readout.
    TopBarScreen screen;
    auto* bars = dynamic_cast<phazite::ui::NumberField*> (phazite::test::findComponentWithTitle (screen.topBar, "Playhead position"));
    auto* time = dynamic_cast<phazite::ui::NumberField*> (phazite::test::findComponentWithTitle (screen.topBar, "Playhead time"));
    REQUIRE (bars != nullptr);
    REQUIRE (time != nullptr);
    CHECK (bars->isVisible());
    CHECK (time->isVisible());
    const auto shown = [] (phazite::ui::NumberField& field) { return field.getTextFromValue (field.getValue()); };
    CHECK (shown (*bars) == juce::String ("1.1.1"));
    CHECK (shown (*time) == juce::String ("0:00.0"));

    // Moved elsewhere: at 120 BPM in 4/4, beat 9.5 is bar 3, its second beat and that beat's third sixteenth, 4.75 seconds in
    // (a clock's tenths, rounded down).
    phazite::movePlayhead (*screen.song, phazite::BeatPosition::fromBeats (9.5));
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return shown (*bars) == "3.2.3"; }));
    CHECK (shown (*time) == juce::String ("0:04.7"));

    // Typed: bar 5's third beat; a minute in; and text that names no place changes nothing.
    bars->setValue (bars->getValueFromText ("5.3"), juce::sendNotificationSync);
    CHECK (phazite::getPlayheadBeat (*screen.song).inBeats() == doctest::Approx (18.0));
    time->setValue (time->getValueFromText ("1:00"), juce::sendNotificationSync);
    CHECK (phazite::getPlayheadBeat (*screen.song).inBeats() == doctest::Approx (120.0));
    CHECK (bars->getValueFromText ("no place") == doctest::Approx (bars->getValue()));
    CHECK (time->getValueFromText ("1:2:3:4") == doctest::Approx (time->getValue()));

    // In 6/8 a beat is an eighth note: beat 3.5 (in quarter notes) is bar 2, its second eighth.
    phazite::setTimeSignature (*screen.song, { 6, 8 });
    phazite::movePlayhead (*screen.song, phazite::BeatPosition::fromBeats (3.5));
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return shown (*bars) == "2.2.1"; }));

    // A narrower bar: the time goes first, then the bars, as the rest of the bar needs its room.
    screen.topBar.setSize (960, screen.topBar.getHeight());
    CHECK (bars->isVisible());
    CHECK_FALSE (time->isVisible());
    screen.topBar.setSize (900, screen.topBar.getHeight());
    CHECK_FALSE (bars->isVisible());
}

TEST_CASE ("Record and the metronome have their options on an arrow joined to each, as a split button")
{
    // The owner's UX audit, 2026-11-12: only a right-click opened them, and nothing showed they were there.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    TopBarScreen screen (300);

    for (const auto& [name, options] : { std::pair { "Record", "Record options" }, std::pair { "Metronome", "Metronome options" } })
    {
        CAPTURE (name);
        auto* button = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (screen.topBar, name));
        auto* arrow = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (screen.topBar, options));
        REQUIRE (button != nullptr);
        REQUIRE (arrow != nullptr);
        CHECK (arrow->isShowing());
        CHECK (arrow->isEnabled());
        CHECK (arrow->getX() == button->getRight());
        CHECK (arrow->getY() == button->getY());
        CHECK (arrow->getHeight() == button->getHeight());
        CHECK (button->isConnectedOnRight());
        CHECK (arrow->isConnectedOnLeft());
        CHECK (arrow->getTooltip().isNotEmpty());

        // A click opens the menu that a right-click on the button opens.
        phazite::test::Mouse mouse (*arrow);
        mouse.click (arrow->getLocalBounds().getCentre());
        CHECK (juce::PopupMenu::dismissAllActiveMenus());
        phazite::test::dispatchPendingMessages();
    }
}
