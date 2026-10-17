// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation lanes in the playlist (src/ui/playlist/PlaylistAutomation.cpp), driven with the mouse as the user would:
// a lane under its track, points added, moved, drawn, painted, selected and deleted, the lane's header, the master's row,
// and a fader's right-click (src/ui/components/Controls.h).
#include "UiTestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Selection.h"
#include "model/automation/AutomationModel.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/DesignTokens.h"

#include <algorithm>

namespace
{
using phazite::AutomationShape;
using phazite::BeatPosition;
using phazite::ParameterRef;

// A playlist in a window with two tracks, the first showing its volume's lane, and the commands and keys the app gives it.
struct LaneScreen
{
    LaneScreen()
    {
        edit->ensureNumberOfAudioTracks (2);
        tracks = te::getAudioTracks (*edit);
        setEdit (context, edit.get());
        volume = phazite::getVolume (context, trackID (0)).getRef();
        phazite::showAutomationLane (context, volume);
        edit->getUndoManager().clearUndoHistory();

        phazite::addSelectionCommands (registry, getSelection (context), getRangeSelection (context));
        phazite::ui::addTimelineCommands (registry, [this] { return &playlist; });
        playlist.addCommands (registry);
        window.addKeyListener (&shortcutKeys); // as the main window does
        phazite::test::dispatchPendingMessages();
    }

    ~LaneScreen()
    {
        window.removeKeyListener (&shortcutKeys);
        setEdit (context, nullptr);
    }

    phazite::TrackID trackID (int track) const { return phazite::getTrackID (*tracks[track]); }

    // The lane's view, which the mouse uses (its tooltip and title name it after its parameter).
    juce::Component& lane (const juce::String& title = "Automation of Volume")
    {
        auto* found = phazite::test::findComponentWithTitle (playlist, title);
        REQUIRE (found != nullptr);
        return *found;
    }

    // Where a beat and a value are in the volume's lane: values from 0 to 1, as the lane draws them.
    juce::Point<int> at (double beat, float value)
    {
        const auto area =
            lane().getLocalBounds().toFloat().withTrimmedBottom (1.0f).reduced (0.0f, phazite::ui::LaneTint::pointRadius + 1.0f);
        return { juce::roundToInt (playlist.getView().beatToX (BeatPosition::fromBeats (beat))),
                 juce::roundToInt (area.getBottom() - value * area.getHeight()) };
    }

    std::vector<phazite::AutomationPoint> getPoints() const { return phazite::getAutomationPoints (context, volume); }

    std::vector<double> getBeats() const
    {
        std::vector<double> beats;

        for (const auto& point : getPoints())
            beats.push_back (point.beat.inBeats());

        return beats;
    }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    juce::Array<te::AudioTrack*> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    ParameterRef volume;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry }; // with the default keymap
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ui::Playlist playlist { commands, context };
    phazite::test::TestWindow window { playlist, 1000, 600 };
};
} // namespace

TEST_CASE ("A lane shows under its track, and a click on it adds a point, on the grid")
{
    LaneScreen screen;
    auto& lane = screen.lane();
    CHECK (lane.isShowing());
    CHECK (lane.getHeight() == phazite::AutomationLane::defaultHeight);

    // Under the first track's row, as wide as the lanes.
    auto* header = phazite::test::findComponentWithTitle (screen.playlist, "Automation lane: Volume");
    REQUIRE (header != nullptr);
    CHECK (header->isShowing());

    phazite::test::Mouse mouse (lane);
    mouse.click (screen.at (4.1, 0.75f));
    const auto points = screen.getPoints();
    REQUIRE (points.size() == 1);
    CHECK (points[0].beat.inBeats() == doctest::Approx (4.0)); // snapped
    CHECK (points[0].value == doctest::Approx (0.75f).epsilon (0.05));
    CHECK (screen.describeUndo() == juce::String ("Undo Add Automation Point"));
    CHECK (screen.context.getSongSelection().getSelectedAutomationPoints().indices == std::vector { 0 });
}

TEST_CASE ("Dragging a point moves it on the grid as one step, and a double-click deletes it")
{
    LaneScreen screen;
    phazite::addAutomationPoint (screen.context, screen.volume, { BeatPosition::fromBeats (4.0), 0.5f });
    phazite::test::dispatchPendingMessages();
    screen.edit->getUndoManager().clearUndoHistory();

    phazite::test::Mouse mouse (screen.lane());
    mouse.press (screen.at (4.0, 0.5f));
    mouse.dragTo (screen.at (5.0, 0.6f));
    mouse.dragTo (screen.at (6.1, 0.75f));
    mouse.release();
    mouse.pause();

    auto points = screen.getPoints();
    REQUIRE (points.size() == 1);
    CHECK (points[0].beat.inBeats() == doctest::Approx (6.0));
    CHECK (points[0].value == doctest::Approx (0.75f).epsilon (0.05));
    CHECK (screen.describeUndo() == juce::String ("Undo Move Automation Points"));

    // One step takes the whole drag back.
    phazite::undo (*screen.edit);
    CHECK (screen.getBeats() == std::vector { 4.0 });
    phazite::redo (*screen.edit);

    mouse.doubleClick (screen.at (6.0, screen.getPoints()[0].value));
    CHECK (screen.getPoints().empty());
}

TEST_CASE ("Dragging a line moves it up or down, and Alt bends it")
{
    LaneScreen screen;
    phazite::addAutomationPoint (screen.context, screen.volume, { BeatPosition::fromBeats (2.0), 0.2f });
    phazite::addAutomationPoint (screen.context, screen.volume, { BeatPosition::fromBeats (6.0), 0.6f });
    phazite::test::dispatchPendingMessages();

    phazite::test::Mouse mouse (screen.lane());

    // The middle of the line: halfway up, at beat 4. Both ends move up by about a tenth.
    mouse.press (screen.at (4.0, 0.4f));
    mouse.dragTo (screen.at (4.0, 0.5f));
    mouse.release();
    mouse.pause();
    auto points = screen.getPoints();
    REQUIRE (points.size() == 2);
    CHECK (points[0].value == doctest::Approx (0.3f).epsilon (0.1));
    CHECK (points[1].value == doctest::Approx (0.7f).epsilon (0.1));
    CHECK (screen.getBeats() == std::vector { 2.0, 6.0 });

    // Alt+drag up bows the rising line up: its curve goes below 0.
    mouse.press (screen.at (4.0, 0.5f), juce::ModifierKeys::altModifier);
    mouse.dragTo (screen.at (4.0, 0.8f));
    mouse.release();
    mouse.pause();
    CHECK (screen.getPoints()[0].curve < -0.1f);
    CHECK (screen.describeUndo() == juce::String ("Undo Bend Automation"));
}

TEST_CASE ("Draw draws steps on the grid, and Paint the chosen shape between where it starts and ends")
{
    LaneScreen screen;
    auto& playlist = screen.playlist;
    phazite::test::Mouse mouse (screen.lane());

    // Draw, at a grid of a beat: a step for each beat the mouse passes, two points each.
    playlist.setGrid ({ .adaptive = false, .fixedLevel = 9 }); // a beat
    playlist.setTool (phazite::ui::Tool::draw);
    mouse.press (screen.at (0.5, 0.25f));
    mouse.dragTo (screen.at (1.5, 0.5f));
    mouse.dragTo (screen.at (2.5, 0.75f));
    mouse.release();
    mouse.pause();
    CHECK (screen.getBeats() == std::vector { 0.0, 1.0, 1.0, 2.0, 2.0, 3.0 });
    CHECK (screen.getPoints()[4].value == doctest::Approx (0.75f).epsilon (0.05));
    CHECK (screen.describeUndo() == juce::String ("Undo Draw Automation"));

    // Paint: a square from beat 4 to beat 8 at a grid of a beat: four cycles, replacing nothing else.
    playlist.setTool (phazite::ui::Tool::paint);
    mouse.press (screen.at (4.0, 0.2f));
    mouse.dragTo (screen.at (8.0, 0.8f));
    mouse.release();
    mouse.pause();
    const auto points = screen.getPoints();
    CHECK (points.size() >= 6 + 8);
    CHECK (screen.getBeats()[0] == doctest::Approx (0.0)); // what Draw drew stays
}

TEST_CASE ("Dragging over empty lane space selects the points in the rectangle, and Delete deletes them")
{
    LaneScreen screen;

    for (const auto beat : { 2.0, 4.0, 6.0 })
        phazite::addAutomationPoint (screen.context, screen.volume, { BeatPosition::fromBeats (beat), 0.5f });

    phazite::test::dispatchPendingMessages();
    phazite::test::Mouse mouse (screen.lane());
    mouse.press (screen.at (3.0, 0.9f));
    mouse.dragTo (screen.at (7.0, 0.1f));
    mouse.release();
    mouse.pause();

    CHECK (screen.context.getSongSelection().getSelectedAutomationPoints().indices == std::vector { 1, 2 });
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::deleteKey));
    screen.window.releaseKeys();
    CHECK (screen.getBeats() == std::vector { 2.0 });
}

TEST_CASE ("A lane's header hides it, and the master's row shows while the master has lanes")
{
    LaneScreen screen;
    auto& context = screen.context;

    auto* hide = phazite::test::findComponentWithTitle (screen.playlist, "Hide lane");
    REQUIRE (hide != nullptr);
    phazite::test::Mouse mouse (*hide);
    mouse.click (hide->getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getAutomationLanes (context, screen.trackID (0)).empty());
    CHECK (phazite::test::findComponentWithTitle (screen.playlist, "Automation of Volume") == nullptr);

    // The master's volume: its lane shows under a row named Master, below the tracks.
    phazite::showAutomationLane (context, phazite::getMasterVolume (context).getRef());
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getMasterRow (context).has_value());
    auto* master = phazite::test::findComponentWithTitle (screen.playlist, "Master");
    REQUIRE (master != nullptr);
    CHECK (master->isShowing());
    CHECK (phazite::test::findComponentWithTitle (screen.playlist, "Automation of Volume") != nullptr);

    // Clicking the master's row selects nothing: it can't be moved or deleted.
    phazite::test::Mouse onMaster (*master);
    onMaster.click (master->getLocalBounds().getCentre());
    CHECK (context.getSongSelection().getTracks().empty());
}

TEST_CASE ("Right-clicking a fader shows its menu instead of moving it")
{
    LaneScreen screen;
    phazite::ui::Fader fader (phazite::ui::Fader::Direction::vertical, "Volume", 0.75);
    phazite::ui::ParameterAttachment attachment (fader);
    attachment.setParameter (phazite::getVolume (screen.context, screen.trackID (1)));
    phazite::test::TestWindow faderWindow (fader, 40, 200);

    auto shown = 0;
    const auto showMenu = std::move (fader.onContextMenu);
    REQUIRE (showMenu != nullptr); // the attachment's menu: Automate and Clear Automation
    fader.onContextMenu = [&shown] { ++shown; };

    const auto before = fader.getValue();
    const auto undoBefore = screen.edit->getUndoManager().getNumActionsInCurrentTransaction();
    const auto stepBefore = screen.describeUndo();
    phazite::test::Mouse mouse (fader);
    mouse.press ({ 20, 50 }, juce::ModifierKeys::rightButtonModifier);
    mouse.dragTo ({ 20, 150 });
    mouse.release();
    CHECK (shown == 1);
    CHECK (fader.getValue() == doctest::Approx (before));
    CHECK (screen.describeUndo() == stepBefore); // no step
    CHECK (screen.edit->getUndoManager().getNumActionsInCurrentTransaction() == undoBefore);
}
