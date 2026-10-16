// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist, driven with the mouse and the keys as the user would.
#include "UiTestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/Selection.h"
#include "model/arrangement/Tracks.h"
#include "model/commands/Undo.h"
#include "model/transport/Playback.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/playlist/TrackHeader.h"
#include "ui/playlist/TrackHeights.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

// A playlist in a window, with three tracks and the commands and keys the app gives it. The mouse uses the tools
// in its lanes.
struct PlaylistScreen
{
    PlaylistScreen()
    {
        edit->ensureNumberOfAudioTracks (3);
        tracks = te::getAudioTracks (*edit);
        edit->getUndoManager().clearUndoHistory();

        phazite::ui::addTimelineCommands (registry, [this] { return &playlist; });
        playlist.addCommands (registry);
        context.setEdit (edit.get());
        window.addKeyListener (&shortcutKeys); // as the main window does
        phazite::test::dispatchPendingMessages();

        lanes = phazite::test::findComponentWithTitle (playlist, "Tracks");
        REQUIRE (lanes != nullptr);
    }

    ~PlaylistScreen()
    {
        window.removeKeyListener (&shortcutKeys);
        context.setEdit (nullptr);
    }

    // How tall a track shows, through the model's row of it.
    int heightOf (int track) const
    {
        return phazite::ui::getTrackHeight (
            phazite::getTrackRow (context, phazite::getTrackID (*tracks[track])).value_or (phazite::TrackRow()));
    }

    // Where a beat is on a track's row, in the lanes (the middle of the row).
    juce::Point<int> at (double beat, int track) const
    {
        auto y = 0;

        for (int i = 0; i < track; ++i)
            y += heightOf (i);

        return { juce::roundToInt (playlist.getView().beatToX (phazite::fromTracktion (beats (beat)))), y + heightOf (track) / 2 };
    }

    std::vector<double> getClipStarts (int track) const
    {
        std::vector<double> starts;

        for (auto* clip : tracks[track]->getClips())
            starts.push_back (clip->getStartBeat().inBeats());

        std::sort (starts.begin(), starts.end());
        return starts;
    }

    te::MidiClip& addClip (int track, double start, double length)
    {
        auto* clip = phazite::insertMidiClip (*tracks[track], { beats (start), beats (start + length) }, nullptr);
        REQUIRE (clip != nullptr);
        return *clip;
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    juce::Array<te::AudioTrack*> tracks;
    phazite::ProjectContext context { phazite::test::getEngine() }; // the open project, as the app has it
    te::SelectionManager& selection = context.getSelection();
    phazite::RangeSelection& range = context.getRangeSelection();
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry }; // with the default keymap
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ui::Playlist playlist { commands, context };
    phazite::test::TestWindow window { playlist, 1000, 600 };
    juce::Component* lanes = nullptr;
};

const juce::KeyPress keyB ('b', 0, 'b'); // the Paint tool's key
} // namespace

TEST_CASE ("The Paint tool goes on painting while its key repeats")
{
    // A held key repeats. Choosing the tool again used to end the drag after a copy or two.
    PlaylistScreen screen;
    screen.selection.selectOnly (screen.addClip (0, 0.0, 4.0)); // the clip the Paint tool places copies of
    phazite::test::dispatchPendingMessages();

    screen.window.pressKey (keyB);
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.press (screen.at (8.1, 0)); // the first copy starts at the grid line before

    for (auto beat = 9.0; beat <= 25.0; beat += 1.0)
    {
        mouse.dragTo (screen.at (beat, 0));
        screen.window.pressKey (keyB);
    }

    mouse.release();

    // The original, and copies end to end from beat 8.
    const auto starts = screen.getClipStarts (0);
    REQUIRE (starts.size() == 6);

    for (size_t i = 1; i < starts.size(); ++i)
        CHECK (starts[i] == doctest::Approx (4.0 + 4.0 * (double) i));
}

TEST_CASE ("A tool's key springs back when it was held while the mouse was used")
{
    PlaylistScreen screen;
    phazite::test::Mouse mouse (*screen.lanes);

    // A tap keeps the tool.
    screen.window.pressKey (keyB);
    screen.window.releaseKeys();
    CHECK (screen.playlist.getTool() == phazite::ui::Tool::paint);

    // A hold while clicking goes back to the tool before when the key is let go.
    REQUIRE (screen.registry.invoke ("tool.smart"));
    screen.window.pressKey (keyB);
    mouse.click (screen.at (20.0, 2));
    CHECK (screen.playlist.getTool() == phazite::ui::Tool::paint);
    screen.window.releaseKeys();
    CHECK (screen.playlist.getTool() == phazite::ui::Tool::smart);

    // Let go during a drag: the drag ends with the tool it began with.
    screen.window.pressKey (juce::KeyPress ('8', 0, '8')); // the Select tool, by its digit
    mouse.press (screen.at (20.0, 2));
    mouse.dragTo (screen.at (24.0, 1));
    screen.window.releaseKeys();
    CHECK (screen.playlist.getTool() == phazite::ui::Tool::select);
    mouse.release();
    CHECK (screen.playlist.getTool() == phazite::ui::Tool::smart);
}

TEST_CASE ("Dragging a clip past the edge of the playlist scrolls it")
{
    PlaylistScreen screen;
    auto& clip = screen.addClip (0, 0.0, 4.0);
    phazite::test::dispatchPendingMessages(); // the playlist shows the new clip
    phazite::test::Mouse mouse (*screen.lanes);

    // The lower half of a clip moves it (the Smart Tool). The pointer stays past the right edge while the
    // lanes scroll. (In the app JUCE keeps sending drags while the mouse is still; here it moves a pixel
    // each time, since JUCE skips a drag that doesn't move.)
    const auto start = screen.at (1.0, 0).translated (0, phazite::ui::TrackHeight::standard / 4);
    mouse.press (start);

    for (int i = 0; i < 20; ++i)
        mouse.dragTo ({ screen.lanes->getWidth() + 40 + i % 2, start.y });

    mouse.release();

    const auto& view = screen.playlist.getView();
    CHECK (view.start.inBeats() > 0.0);

    // The clip went along, to beyond what showed when the drag began.
    const auto widthInBeats = screen.lanes->getWidth() / view.pixelsPerBeat;
    CHECK (clip.getStartBeat().inBeats() > widthInBeats);
}

TEST_CASE ("A notch of the mouse wheel zooms or scrolls the playlist once")
{
    // The playlist used to hear about the wheel twice, and zoomed or scrolled two steps for one.
    PlaylistScreen screen;
    phazite::test::Mouse mouse (*screen.lanes);
    const auto notch = 60.0f / 256.0f; // as JUCE reports one on Windows

    const auto before = screen.playlist.getView().pixelsPerBeat;
    mouse.wheel (screen.at (8.0, 0), notch, juce::ModifierKeys::ctrlModifier);
    CHECK (screen.playlist.getView().pixelsPerBeat == doctest::Approx (before * 1.25));

    // Shift+wheel scrolls sideways, by 48 pixels a notch.
    const auto startBefore = screen.playlist.getView().start.inBeats();
    mouse.wheel (screen.at (8.0, 0), -notch, juce::ModifierKeys::shiftModifier);
    const auto& view = screen.playlist.getView();
    CHECK ((view.start.inBeats() - startBefore) * view.pixelsPerBeat == doctest::Approx (48.0));
}

TEST_CASE ("The row after the last track, and a double-click under it, add tracks at the end")
{
    PlaylistScreen screen;
    auto* row = phazite::test::findComponentWithTitle (screen.playlist, "Add track at the end");
    REQUIRE (row != nullptr);
    CHECK (row->isShowing());

    phazite::test::Mouse rowMouse (*row);
    rowMouse.click (row->getLocalBounds().getCentre());
    CHECK (te::getAudioTracks (*screen.edit).size() == 4);

    // The lanes are taller than four tracks.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.doubleClick ({ 100, 4 * phazite::ui::TrackHeight::standard + 40 });
    CHECK (te::getAudioTracks (*screen.edit).size() == 5);

    // Each is an undo step.
    screen.edit->getUndoManager().undo();
    CHECK (te::getAudioTracks (*screen.edit).size() == 4);
}

TEST_CASE ("The playlist paints, with sections, a clip and a selected track")
{
    PlaylistScreen screen;
    REQUIRE (phazite::findOutline ("pop") != nullptr);
    phazite::applyOutline (*screen.edit, *phazite::findOutline ("pop"));
    screen.addClip (1, 4.0, 8.0);
    screen.selection.selectOnly (screen.tracks[2]);
    phazite::test::dispatchPendingMessages();

    const auto image = screen.window.paintEverything();
    CHECK (image.getWidth() == 1000);
}

TEST_CASE ("Only a track's handle moves it, its arrow collapses it, and its edge resizes it")
{
    PlaylistScreen screen;
    auto headers = phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist);
    REQUIRE (headers.size() == 3);
    auto& header = *headers[0];
    REQUIRE (header.getTrack() == phazite::getTrackID (*screen.tracks[0]));

    phazite::test::Mouse mouse (screen.playlist);
    const auto inPlaylist = [&screen, &header] (juce::Point<int> p) { return screen.playlist.getLocalPoint (&header, p); };
    const auto down = juce::Point<int> (0, 2 * phazite::ui::TrackHeight::standard + 40); // near the boundary under the third track

    // Dragging the middle of the header moves nothing: it selects the tracks the mouse passes.
    const auto middle = inPlaylist (header.getLocalBounds().getCentre());
    mouse.press (middle);
    mouse.dragTo (middle + down);
    mouse.release();
    phazite::test::dispatchPendingMessages();
    CHECK (te::getAudioTracks (*screen.edit) == screen.tracks);
    CHECK (phazite::getSelectedTracks (screen.selection).size() == 3);

    // Dragging the handle moves the track (selected alone by a click first): it goes last.
    mouse.click (middle);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getSelectedTracks (screen.selection).size() == 1);
    const auto grip = inPlaylist (header.getGripArea().getCentre());
    mouse.press (grip);
    mouse.dragTo (grip + down);
    mouse.release();
    phazite::test::dispatchPendingMessages();
    CHECK (te::getAudioTracks (*screen.edit).getLast() == screen.tracks[0]);

    // The arrow collapses the track to a thin row, which shows its name.
    auto* collapse = phazite::test::findComponentWithTitle (header, "Collapse track");
    REQUIRE (collapse != nullptr);
    mouse.click (inPlaylist (collapse->getBounds().getCentre()));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::isCollapsed (*screen.tracks[0]));
    CHECK (header.getHeight() == phazite::ui::TrackHeight::collapsed);
    CHECK (phazite::test::findComponentWithTitle (header, "Expand track") != nullptr);

    // Dragging its bottom edge down expands it to where the edge is let go.
    const auto edge = inPlaylist ({ header.getWidth() / 2, header.getHeight() - 2 });
    mouse.press (edge);
    mouse.dragTo (edge + juce::Point<int> (0, 30));
    mouse.release();
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::isCollapsed (*screen.tracks[0]));
    CHECK (header.getHeight() == phazite::ui::TrackHeight::collapsed + 30);
}

TEST_CASE ("With FL Studio's keymap, a click on empty space places the last clip used, and right-click deletes it")
{
    PlaylistScreen screen;
    screen.commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("FL Studio"));
    auto& clip = screen.addClip (0, 0.0, 4.0);
    phazite::test::dispatchPendingMessages();
    screen.selection.selectOnly (clip); // the clip used last
    phazite::test::dispatchPendingMessages();

    phazite::test::Mouse mouse (*screen.lanes);
    mouse.click (screen.at (8.05, 1));
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.getClipStarts (1).size() == 1);
    CHECK (screen.getClipStarts (1)[0] == doctest::Approx (8.0).epsilon (0.01)); // from the grid line before the click

    mouse.click (screen.at (9.0, 1), juce::ModifierKeys::rightButtonModifier);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getClipStarts (1).empty());

    // In Phazite's own keymap, a click on empty space selects the track, and places nothing.
    screen.commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("Phazite"));
    mouse.click (screen.at (8.05, 1));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getClipStarts (1).empty());
    CHECK (screen.selection.isSelected (screen.tracks[1]));
}

#if JUCE_WINDOWS
TEST_CASE ("With FL Studio's keymap, a click places the clip selected just before, also while another thread's wake-up is late")
{
    // As CI once saw: the selection's change message, which tells the playlist which clip was used last, queued behind
    // another thread's message whose wake-up wasn't posted yet, so it got none of its own, and the click came first.
    PlaylistScreen screen;
    screen.commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("FL Studio"));
    auto& clip = screen.addClip (0, 0.0, 4.0);
    auto lateWakeUp = phazite::test::queueWithoutWakeUp (100); // delivers what is waiting first
    screen.selection.selectOnly (clip);
    phazite::test::dispatchPendingMessages();

    phazite::test::Mouse mouse (*screen.lanes);
    mouse.click (screen.at (8.05, 1));
    lateWakeUp.join(); // before this was fixed, the selection's message came only after this
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.getClipStarts (1).size() == 1);
    CHECK (screen.getClipStarts (1)[0] == doctest::Approx (8.0).epsilon (0.01));
}
#endif

TEST_CASE ("With FL Studio's keymap, a selected time range loops while it is selected")
{
    PlaylistScreen screen;
    screen.commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("FL Studio"));
    screen.addClip (0, 0.0, 16.0);
    phazite::test::dispatchPendingMessages();
    auto& transport = screen.edit->getTransport();

    // Ctrl+drag over empty space selects a time range (a plain click would place a clip), and it loops.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.press (screen.at (20.0, 1), juce::ModifierKeys::commandModifier);
    mouse.dragTo (screen.at (28.0, 1));
    mouse.release();
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (screen.range.isEmpty());
    CHECK (transport.looping.get());
    CHECK_FALSE (phazite::doesLoopFollowSong (*screen.edit));
    const auto loop = screen.edit->tempoSequence.toBeats (transport.getLoopRange());
    CHECK (loop.getStart().inBeats() == doctest::Approx (screen.range.getRange().getStart().inBeats()));
    CHECK (loop.getEnd().inBeats() == doctest::Approx (screen.range.getRange().getEnd().inBeats()));

    // Without a selection, the song loops again.
    mouse.click (screen.at (2.0, 2), juce::ModifierKeys::commandModifier);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.range.isEmpty());
    CHECK (phazite::doesLoopFollowSong (*screen.edit));
    CHECK (screen.edit->tempoSequence.toBeats (transport.getLoopRange()).getEnd().inBeats() == doctest::Approx (16.0));
}

TEST_CASE ("With FL Studio's keymap, Ctrl+B repeats a selected time range, and the loop follows it")
{
    PlaylistScreen screen;
    phazite::addSelectionCommands (screen.registry, screen.selection, screen.range);
    screen.commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("FL Studio"));
    screen.addClip (0, 0.0, 4.0);
    phazite::test::dispatchPendingMessages();

    // Two bars on the first two tracks; in this keymap the range loops while it is selected.
    screen.range.set ({ beats (0.0), beats (8.0) }, { screen.tracks[0], screen.tracks[1] });
    phazite::test::dispatchPendingMessages();
    auto& transport = screen.edit->getTransport();
    REQUIRE (transport.looping.get());

    screen.window.pressKey (juce::KeyPress::createFromDescription ("command + B"));
    phazite::test::dispatchPendingMessages();

    // What was in the range is there again right after it, the range is the copy, and the loop went with it.
    const auto starts = screen.getClipStarts (0);
    REQUIRE (starts.size() == 2);
    CHECK (starts[1] == doctest::Approx (8.0));
    CHECK (screen.getClipStarts (1).empty());
    CHECK (screen.edit->getUndoManager().getUndoDescription() == juce::String ("Duplicate Range"));
    CHECK (screen.range.getRange().getStart().inBeats() == doctest::Approx (8.0));
    CHECK (screen.range.getRange().getEnd().inBeats() == doctest::Approx (16.0));
    const auto loop = screen.edit->tempoSequence.toBeats (transport.getLoopRange());
    CHECK (loop.getStart().inBeats() == doctest::Approx (8.0));
    CHECK (loop.getEnd().inBeats() == doctest::Approx (16.0));

    // Undo takes the copy back and puts the range back, and the loop follows the range there.
    phazite::undo (*screen.edit);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getClipStarts (0).size() == 1);
    CHECK (screen.range.getRange().getStart().inBeats() == doctest::Approx (0.0));
    CHECK (screen.range.getRange().getEnd().inBeats() == doctest::Approx (8.0));
    const auto loopAfterUndo = screen.edit->tempoSequence.toBeats (transport.getLoopRange());
    CHECK (loopAfterUndo.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (loopAfterUndo.getEnd().inBeats() == doctest::Approx (8.0));
}

TEST_CASE ("The playlist's vertical scroll bar shows only while its tracks don't fit, and its room goes to the tracks otherwise")
{
    PlaylistScreen screen;                         // three tracks, which fit
    CHECK (screen.playlist.isFollowingPlayback()); // unlike the piano roll
    juce::ScrollBar* vertical = nullptr;

    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
        if (bar->isVertical())
            vertical = bar;

    REQUIRE (vertical != nullptr);
    CHECK_FALSE (vertical->isVisible());
    CHECK (screen.lanes->getRight() == screen.playlist.getWidth());

    screen.edit->ensureNumberOfAudioTracks (30);
    phazite::test::dispatchPendingMessages();
    CHECK (vertical->isVisible());
    CHECK (screen.lanes->getRight() == screen.playlist.getWidth() - phazite::ui::TimelineEditor::scrollBarSize);

    for (auto* track : te::getAudioTracks (*screen.edit))
        if (! screen.tracks.contains (track))
            screen.edit->deleteTrack (track);

    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (vertical->isVisible());
    CHECK (screen.lanes->getRight() == screen.playlist.getWidth());
}

TEST_CASE ("Every clip shows in its track's color, and the rows themselves aren't colored")
{
    PlaylistScreen screen;
    auto& clip = screen.addClip (0, 0.0, 4.0);
    clip.setColour (juce::Colours::red); // a color of its own, as Tracktion gives some clips
    phazite::test::dispatchPendingMessages();

    const auto clipComponents = phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist);
    REQUIRE (clipComponents.size() == 1);
    CHECK (clipComponents[0]->getColour() == phazite::getTrackColour (*screen.tracks[0], 0));

    // An empty stretch of an unselected track's row is the playlist's background, between grid lines.
    const auto image = screen.window.paintEverything();
    const auto point = screen.lanes->getBounds().getPosition() + screen.at (10.37, 1);
    CHECK (image.getPixelAt (point.x, point.y) == phazite::ui::getPalette (screen.playlist).background);
}

TEST_CASE ("Dragging along the top of the ruler loops part of the song, and the loop's edges and middle change it")
{
    PlaylistScreen screen;
    screen.addClip (0, 0.0, 16.0); // four bars
    phazite::test::dispatchPendingMessages();

    auto* ruler = dynamic_cast<phazite::ui::TimelineRuler*> (phazite::test::findComponentWithTitle (screen.playlist, "Timeline"));
    REQUIRE (ruler != nullptr);
    const auto lane = ruler->getLoopLane().getCentreY(), lower = ruler->getHeight() - 4;
    const auto x = [&screen] (double beat)
    { return juce::roundToInt (screen.playlist.getView().beatToX (phazite::fromTracktion (beats (beat)))); };
    const auto isLoop = [&screen] (double start, double end)
    {
        const auto loop = phazite::getLoopBeatRange (*screen.edit);
        return loop.has_value() && std::abs (loop->first - start) < 1.0e-6 && std::abs (loop->second - end) < 1.0e-6;
    };

    phazite::test::Mouse mouse (*ruler);

    // A drag along the top loops what it covers, on the grid.
    mouse.press ({ x (4.1), lane });
    mouse.dragTo ({ x (8.1), lane });
    mouse.release();
    CHECK (isLoop (4.0, 8.0));
    CHECK_FALSE (phazite::doesLoopFollowSong (*screen.edit));

    // Its end, then its middle.
    mouse.press ({ x (8.0), lane });
    mouse.dragTo ({ x (12.0), lane });
    mouse.release();
    CHECK (isLoop (4.0, 12.0));
    mouse.press ({ x (6.0), lane });
    mouse.dragTo ({ x (8.0), lane });
    mouse.release();
    CHECK (isLoop (6.0, 14.0));
    mouse.pause();

    // A click there turns looping off and on again; a double-click loops the whole song.
    mouse.click ({ x (20.0), lane });
    CHECK_FALSE (phazite::getLoopBeatRange (*screen.edit).has_value());
    mouse.click ({ x (20.0), lane });
    CHECK (isLoop (6.0, 14.0));
    mouse.doubleClick ({ x (20.0), lane });
    CHECK (phazite::isLoopingSong (*screen.edit));

    // Lower down, a click moves the playhead and leaves the loop alone; Ctrl+drag there makes a loop, as in FL Studio.
    mouse.click ({ x (3.0), lower });
    CHECK (screen.playlist.getPlayheadBeat().inBeats() == doctest::Approx (3.0));
    CHECK (phazite::isLoopingSong (*screen.edit));
    mouse.press ({ x (1.0), lower }, juce::ModifierKeys::commandModifier);
    mouse.dragTo ({ x (2.0), lower });
    mouse.release();
    CHECK (isLoop (1.0, 2.0));
}
