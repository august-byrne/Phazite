// SPDX-License-Identifier: AGPL-3.0-or-later
// The tempo lane in the playlist (src/ui/playlist/TempoLane.h) and its header, driven as the user would: the mouse on the
// lane, and the items of the menus it and the time bar show.
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
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
        const auto* item = findMenuItem (screen.header->getMoreMenu(), { name });
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
