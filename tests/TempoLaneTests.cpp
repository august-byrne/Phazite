// SPDX-License-Identifier: AGPL-3.0-or-later
// The tempo lane in the playlist (src/ui/playlist/TempoLane.h) and its header, driven as the user would: the mouse on the
// lane, and the items of the menus it and the time bar show.
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "ui/components/ValueText.h"
#include "ui/playlist/Playlist.h"
#include "ui/playlist/TempoLane.h"

#include <cmath>

namespace
{
phazite::BeatPosition beat (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

using phazite::test::findMenuItem;
using phazite::test::runMenuItem;

bool isWhole (double bpm)
{
    return std::abs (bpm - std::round (bpm)) < 1.0e-9;
}

// A playlist in a window, with two tracks and the tempo's commands, as the app shows it.
struct TempoScreen
{
    TempoScreen()
    {
        phazite::ensureTracks (*song, 2);
        phazite::ui::addTimelineCommands (registry, [this] { return &playlist; });
        playlist.addCommands (registry);
        setSong (context, song.get());
        phazite::addTempoCommands (registry, phazite::getSelection (context));
        phazite::test::dispatchPendingMessages();

        lane = dynamic_cast<phazite::ui::TempoLane*> (phazite::test::findComponentWithTitle (playlist, "Tempo"));
        header = dynamic_cast<phazite::ui::TempoLaneHeader*> (phazite::test::findComponentWithTitle (playlist, "Tempo lane"));
        sections = phazite::test::findAll<phazite::ui::SectionsStrip> (playlist).getFirst();
        REQUIRE (lane != nullptr);
        REQUIRE (header != nullptr);
        REQUIRE (sections != nullptr);
    }

    ~TempoScreen() { setSong (context, nullptr); }

    std::vector<phazite::TempoChange> tempos() const { return phazite::getTempoChanges (*song); }

    juce::Point<int> point (int index) const { return lane->getPointPosition (index).roundToInt(); }
    int xOf (double beats) const { return juce::roundToInt (playlist.getView().beatToX (beat (beats))); }

    phazite::Song::Ptr song = phazite::createSong(); // 120 BPM, 4/4
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Playlist playlist { commands, context };
    phazite::test::TestWindow window { playlist, 1000, 600 };
    phazite::ui::TempoLane* lane = nullptr;
    phazite::ui::TempoLaneHeader* header = nullptr;
    phazite::ui::SectionsStrip* sections = nullptr; // whose menu the time bar's right-click shows
};
} // namespace

TEST_CASE ("The tempo lane shows only while the song's tempo changes: the time bar's menu adds the first change")
{
    TempoScreen screen;
    CHECK_FALSE (screen.lane->isVisible());
    CHECK_FALSE (screen.header->isVisible());

    // From the time bar's right-click, on the grid, at the tempo there.
    REQUIRE (runMenuItem (screen.sections->getMenuAt (beat (16.0)), { "Add Tempo Change Here" }));
    REQUIRE (screen.tempos().size() == 2);
    CHECK (screen.tempos()[1].beat == beat (16.0));
    CHECK (screen.tempos()[1].bpm == doctest::Approx (120.0));
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Add Tempo Change"));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.lane->isVisible());
    CHECK (screen.header->isVisible());
    CHECK (screen.lane->getHeight() == phazite::ui::TempoLane::preferredHeight);
    CHECK (screen.lane->getDescription().startsWith ("The song's tempo"));

    // It shows its tempos with room above and below; a point on each.
    CHECK (screen.lane->getShownTempos().contains (120.0));
    CHECK (screen.lane->getPointAt (screen.lane->getPointPosition (1)) == 1);

    // Its header's menu offers Remove Tempo Changes, which takes the lane away (its commands run as the menu would).
    const auto offers = [&screen] (const juce::String& name)
    {
        const auto menu = screen.header->getMoreMenu(); // kept while its item is read: the menu is made anew each time
        const auto* item = findMenuItem (menu, { name });
        return item != nullptr && item->isEnabled;
    };

    REQUIRE (offers ("Remove Tempo Changes"));
    REQUIRE (screen.registry.invoke ("transport.removeTempoChanges"));
    CHECK_FALSE (phazite::hasTempoChanges (*screen.song));
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (screen.lane->isVisible());
    CHECK_FALSE (screen.header->isVisible());

    // And Add Tempo Change, which adds one where the playhead is.
    phazite::movePlayhead (*screen.song, beat (8.0));
    REQUIRE (offers ("Add Tempo Change"));
    REQUIRE (screen.registry.invoke ("transport.addTempoChange"));
    REQUIRE (screen.tempos().size() == 2);
    CHECK (screen.tempos()[1].beat == beat (8.0));
}

TEST_CASE ("The tempo lane: a point drags, a line ramps and bends, a click adds a tempo change, and a double-click deletes one")
{
    TempoScreen screen;
    phazite::addTempoChange (*screen.song, beat (8.0), 140.0);
    screen.song->getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.lane->isVisible());
    phazite::test::Mouse mouse (*screen.lane);

    // Dragged later and up: on the grid, by whole beats per minute, the lane's tempos as they were until it lets go.
    const auto shownBefore = screen.lane->getShownTempos();
    const auto from = screen.point (1);
    mouse.press (from);
    mouse.dragTo (from + juce::Point<int> (screen.xOf (4.0) - screen.xOf (0.0), -9));
    CHECK (screen.lane->getShownTempos() == shownBefore);
    mouse.release();
    mouse.pause();
    REQUIRE (screen.tempos().size() == 2);
    CHECK (screen.tempos()[1].beat == beat (12.0));
    CHECK (screen.tempos()[1].bpm > 140.0);
    CHECK (isWhole (screen.tempos()[1].bpm));
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Move Tempo Change"));

    // The first moves up and down only.
    const auto first = screen.point (0);
    mouse.press (first);
    mouse.dragTo (first + juce::Point<int> (60, 5));
    mouse.release();
    mouse.pause();
    CHECK (screen.tempos()[0].beat == beat (0.0));
    CHECK (screen.tempos()[0].bpm < 120.0);
    phazite::undo (*screen.song);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.tempos()[0].bpm == doctest::Approx (120.0));

    // Its line made a ramp from the menu: straight from one tempo to the next.
    const juce::Point<float> onLine ((float) screen.xOf (6.0), screen.lane->getPointPosition (0).y);
    REQUIRE (runMenuItem (screen.lane->getMenuAt (onLine), { "Ramp to the Next Tempo" }));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.tempos()[0].curve == doctest::Approx (phazite::tempoRamp));
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Change Tempo Ramp"));
    CHECK (phazite::getTempoAt (*screen.song, beat (6.0)) == doctest::Approx ((120.0 + screen.tempos()[1].bpm) / 2.0));

    // Bent at its handle, dragged up: a rising ramp bows up, quick at first.
    const auto handle = screen.lane->getHandle (0);
    REQUIRE (handle.has_value());
    CHECK (screen.lane->getHandleAt (*handle) == 0);
    mouse.press (handle->roundToInt());
    mouse.dragTo (handle->roundToInt() - juce::Point<int> (0, 10));
    mouse.release();
    mouse.pause();
    CHECK (screen.tempos()[0].curve < -0.1f);
    CHECK (screen.tempos()[0].curve >= -0.5f);
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Bend Tempo Ramp"));

    // Double-clicked, the handle straightens the ramp.
    const auto bent = screen.lane->getHandle (0);
    REQUIRE (bent.has_value());
    mouse.doubleClick (bent->roundToInt());
    CHECK (screen.tempos()[0].curve == doctest::Approx (phazite::tempoRamp));

    // Held again from the menu: a step, which has no handle.
    REQUIRE (runMenuItem (screen.lane->getMenuAt (onLine), { "Hold Until the Next Tempo" }));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.tempos()[0].curve == doctest::Approx (phazite::tempoJump));
    CHECK_FALSE (screen.lane->getHandle (0).has_value());

    // A click on empty space after the last point: a tempo change there, on the grid, at a whole tempo.
    mouse.click ({ screen.xOf (20.0), 1 });
    REQUIRE (screen.tempos().size() == 3);
    CHECK (screen.tempos()[2].beat == beat (20.0));
    CHECK (isWhole (screen.tempos()[2].bpm));
    CHECK (screen.tempos()[2].bpm > screen.tempos()[1].bpm); // clicked near the lane's top
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Add Tempo Change"));
    phazite::test::dispatchPendingMessages();

    // Double-clicked, a point goes; the first can't.
    mouse.doubleClick (screen.point (2));
    CHECK (screen.tempos().size() == 2);
    phazite::test::dispatchPendingMessages();
    const auto firstMenu = screen.lane->getMenuAt (screen.lane->getPointPosition (0));
    const auto* deleteFirst = findMenuItem (firstMenu, { "Delete Tempo Change" });
    REQUIRE (deleteFirst != nullptr);
    CHECK_FALSE (deleteFirst->isEnabled);
    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.lane->getPointPosition (1)), { "Delete Tempo Change" }));
    CHECK_FALSE (phazite::hasTempoChanges (*screen.song));
}

TEST_CASE ("The tempo lane's drags, clicks and menu items are each one exact undo step, and the walk back and forth gives each song "
           "again")
{
    // The lane's gestures through the undo check (UndoCheck.h), which also compiles a plan after each change, its Undo and its
    // Redo, and compares what the plan kept with the song: a tempo change the plan's compiler wasn't told of shows there.
    TempoScreen screen;
    phazite::addTempoChange (*screen.song, beat (8.0), 140.0);
    screen.song->getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.lane->isVisible());
    phazite::test::Mouse mouse (*screen.lane);
    phazite::test::UndoCheck undo (screen.context);

    const auto drag = [&mouse] (juce::Point<int> from, juce::Point<int> by)
    {
        mouse.press (from);
        mouse.dragTo (from + by);
        mouse.release();
        mouse.pause();
    };

    undo.check ("Move Tempo Change", [&] { drag (screen.point (1), { screen.xOf (4.0) - screen.xOf (0.0), -9 }); });
    CHECK (screen.tempos()[1].beat == beat (12.0));
    undo.check ("Change the First Tempo", [&] { drag (screen.point (0), { 0, 5 }); });
    CHECK (screen.tempos()[0].bpm < 120.0);

    const juce::Point<float> onLine ((float) screen.xOf (6.0), screen.lane->getPointPosition (0).y);
    undo.check ("Ramp to the Next Tempo", [&] { REQUIRE (runMenuItem (screen.lane->getMenuAt (onLine), { "Ramp to the Next Tempo" })); });
    phazite::test::dispatchPendingMessages();
    CHECK (screen.tempos()[0].curve == doctest::Approx (phazite::tempoRamp));

    const auto handle = screen.lane->getHandle (0);
    REQUIRE (handle.has_value());
    undo.check ("Bend Tempo Ramp", [&] { drag (handle->roundToInt(), { 0, -10 }); });
    CHECK (screen.tempos()[0].curve < -0.1f);

    undo.check ("Add Tempo Change", [&] { mouse.click ({ screen.xOf (20.0), 1 }); });
    REQUIRE (screen.tempos().size() == 3);
    phazite::test::dispatchPendingMessages();
    undo.check ("Delete Tempo Change", [&] { mouse.doubleClick (screen.point (2)); });
    CHECK (screen.tempos().size() == 2);
    phazite::test::dispatchPendingMessages();

    undo.walkBackAndForth();
}

TEST_CASE ("A tempo change's menu types its tempo in a field over its point, as one step")
{
    // The owner, 2026-11-13: "Same thing for the tempo automation. should be able to type a value", as an automation point's
    // menu types its value.
    TempoScreen screen;
    phazite::addTempoChange (*screen.song, beat (8.0), 140.0);
    screen.song->getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.lane->isVisible());

    // Its point's menu: Type a Value..., which opens a field over the point with its tempo, all of it selected.
    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.lane->getPointPosition (1)), { "Type a Value..." }));
    auto* field = screen.lane->getTempoField();
    REQUIRE (field != nullptr);
    CHECK (field->isShowing());
    CHECK (field->hasKeyboardFocus (false));
    CHECK (field->getText() == phazite::ui::formatTempo (140.0));
    CHECK (field->getHighlightedRegion().getLength() == field->getText().length());
    const auto pointInPlaylist = screen.playlist.getLocalPoint (screen.lane, screen.lane->getPointPosition (1));
    CHECK (screen.playlist.getLocalArea (field->getParentComponent(), field->getBounds()).getCentreX()
           == doctest::Approx (pointInPlaylist.x).epsilon (0.01));

    // "128.5" and Enter: that tempo from the same beat, as one step.
    for (const auto key : { '1', '2', '8', '.', '5' })
        screen.window.pressKey (juce::KeyPress (key, {}, (juce::juce_wchar) key));

    screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (screen.lane->getTempoField() == nullptr);
    REQUIRE (screen.tempos().size() == 2);
    CHECK (screen.tempos()[1].beat == beat (8.0));
    CHECK (screen.tempos()[1].bpm == doctest::Approx (128.5));
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Change Tempo"));
    phazite::test::dispatchPendingMessages(); // the field goes, a message after it closed

    // The first tempo too. Esc leaves it as it was, and a tempo past the limits is the limit.
    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.lane->getPointPosition (0)), { "Type a Value..." }));
    REQUIRE (screen.lane->getTempoField() != nullptr);
    screen.lane->getTempoField()->setText ("90", false);
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    CHECK (screen.tempos()[0].bpm == doctest::Approx (120.0));
    phazite::test::dispatchPendingMessages();

    REQUIRE (runMenuItem (screen.lane->getMenuAt (screen.lane->getPointPosition (0)), { "Type a Value..." }));
    REQUIRE (screen.lane->getTempoField() != nullptr);
    screen.lane->getTempoField()->setText ("999 bpm", false);
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (screen.tempos()[0].bpm == doctest::Approx (phazite::maxTempo));
    CHECK (screen.tempos()[1].bpm == doctest::Approx (128.5));

    // One undo puts it back.
    phazite::undo (*screen.song);
    CHECK (screen.tempos()[0].bpm == doctest::Approx (120.0));
}

TEST_CASE ("A tempo change's number stays where it is as its point is selected")
{
    // The owner, 2026-11-16: "when i click on a automation point on the playlist, it's number jumps up because the selected
    // bubble is bigger than the point itself". The number was placed from the dot's size, which grows as it is selected.
    TempoScreen screen;
    phazite::addTempoChange (*screen.song, beat (8.0), 140.0);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.lane->isVisible());

    // Right of the selected dot and its ring, where the number is, the pixels before and after the click are the same.
    const auto centre = screen.lane->getPointPosition (1);
    const auto clear = (int) std::ceil (centre.x + phazite::ui::LaneTint::selectedPointRadius + phazite::ui::LaneTint::selectedRing) + 1;
    const auto label = juce::Rectangle<int> (clear, 0, 48, screen.lane->getHeight());
    const auto before = screen.lane->createComponentSnapshot (label);

    phazite::test::Mouse mouse (*screen.lane);
    mouse.click (screen.point (1));
    mouse.moveTo ({ screen.point (1).x, screen.lane->getHeight() + 40 }); // off the lane, so that nothing shows for the pointer
    phazite::test::dispatchPendingMessages();
    const auto after = screen.lane->createComponentSnapshot (label);

    auto differing = 0;

    for (int y = 0; y < label.getHeight(); ++y)
        for (int x = 0; x < label.getWidth(); ++x)
            differing += before.getPixelAt (x, y) != after.getPixelAt (x, y) ? 1 : 0;

    CHECK (differing == 0);
}
