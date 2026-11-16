// SPDX-License-Identifier: AGPL-3.0-or-later
// Every tool of the playlist and the piano roll used with the mouse, as the user uses it (ui/timeline/SmartTool.h): Slice,
// Slip, Mute, Delete, Select and Zoom, and the Smart Tool's trims, fades, crossfades and aliases. Each checks what the song
// holds after it, the undo step's name, and that Undo and Redo give the song back exactly (UndoCheck.h); and the cases a
// hand meets: an item off the grid, a drag that leaves the view and comes back, a tool's key pressed in the middle of a
// drag, and a drag let go outside the window.
#include "EditorScreens.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/mixer/MixerModel.h"
#include "ui/components/Controls.h"
#include "ui/playlist/TrackHeader.h"
#include "ui/theme/DesignTokens.h"

namespace
{
using phazite::BeatPosition;
using phazite::test::beatAt;
using phazite::test::getKey;
using phazite::test::Mouse;
using phazite::test::PianoRollScreen;
using phazite::test::PlaylistScreen;
using phazite::test::UndoCheck;
using phazite::ui::Tool;

const auto ctrl = juce::ModifierKeys (juce::ModifierKeys::commandModifier);
const auto alt = juce::ModifierKeys (juce::ModifierKeys::altModifier);

// How far from a beat the mouse can land, on the editor's whole pixels.
double pixelInBeats (const phazite::ui::TimelineEditor& editor)
{
    return 1.0 / editor.getView().pixelsPerBeat;
}

// A point outside the test window, left of it, at the height of a point in a component.
juce::Point<int> leftOfWindow (const phazite::test::TestWindow& window, const juce::Component& component, juce::Point<int> at)
{
    const auto inWindow = window.getLocalPoint (&component, at);
    return component.getLocalPoint (&window, juce::Point<int> (-40, inWindow.y));
}
} // namespace

//==============================================================================
namespace
{
// A track's header in the playlist, and a place on it that no button, field, edge or the color strip at its left takes: where a
// click selects the track.
juce::Component* findHeader (phazite::ui::Playlist& playlist, phazite::TrackID track)
{
    for (auto* header : phazite::test::findAll<phazite::ui::TrackHeader> (playlist))
        if (header->getTrack() == track)
            return header;

    return nullptr;
}

juce::Point<int> getPlainSpot (juce::Component& component)
{
    for (int y = 4; y < component.getHeight() - 8; y += 2)
        for (int x = component.getWidth() / 3; x < component.getWidth() - 4; x += 2)
            if (component.getComponentAt (x, y) == &component)
                return { x, y };

    return {};
}

double getRowsScroll (phazite::ui::Playlist& playlist)
{
    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (playlist))
        if (bar->isVertical())
            return bar->getCurrentRangeStart();

    return -1.0;
}
} // namespace

TEST_CASE ("A Shift+click or Ctrl+click on a track far from the one selected leaves the playlist where it is; a command that "
           "selects a track out of view brings it into view (known bug 54)")
{
    // Every change of the selection scrolled back to the first selected track's row, hiding what was clicked.
    PlaylistScreen screen;
    const auto tracks = phazite::ensureTracks (screen.getSong(), 40);
    phazite::test::dispatchPendingMessages();
    Mouse mouse (screen.playlist);
    const auto onHeader = [&screen] (phazite::TrackID track)
    {
        auto* header = findHeader (screen.playlist, track);
        REQUIRE (header != nullptr);
        REQUIRE (header->isShowing());
        return screen.playlist.getLocalPoint (header, getPlainSpot (*header));
    };
    // The rows scrolled as their scroll bar scrolls them.
    const auto scrollTo = [&screen] (double y)
    {
        for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
            if (bar->isVertical())
                bar->setCurrentRangeStart (y, juce::sendNotificationSync);

        phazite::test::deliverChanges(); // the headers a jump leaves for later, before the next click (Playlist.h)
    };
    auto scrolled = 0.0;

    for (const auto mods : { juce::ModifierKeys (juce::ModifierKeys::shiftModifier), ctrl })
    {
        // The first track clicked, in view; the view scrolled down a long way.
        mouse.click (onHeader (tracks[0]));
        phazite::test::deliverChanges();
        REQUIRE (screen.selection().getTracks() == std::vector { tracks[0] });
        scrollTo (960.0);
        scrolled = getRowsScroll (screen.playlist);
        REQUIRE (scrolled > 0.0);

        mouse.click (onHeader (tracks[24]), mods);
        phazite::test::deliverChanges();

        CHECK (screen.selection().isSelected (tracks[24]));
        CHECK (screen.selection().isSelected (tracks[0]));
        CHECK (getRowsScroll (screen.playlist) == doctest::Approx (scrolled));

        // Back to the top for the next round: the first track's row in view.
        scrollTo (0.0);
    }

    // A command that selects the next track, the first's, out of view: it comes into view.
    mouse.click (onHeader (tracks[0]));
    phazite::test::deliverChanges();
    scrollTo (960.0);
    scrolled = getRowsScroll (screen.playlist);
    REQUIRE (scrolled > 0.0);
    REQUIRE (screen.registry.invoke ("track.selectNext"));
    phazite::test::deliverChanges();
    CHECK (screen.selection().getTracks() == std::vector { tracks[1] });
    CHECK (getRowsScroll (screen.playlist) < scrolled);

    // The command onto a track half hidden at the lanes' foot: it comes into view whole. Counted in view by any pixel of it, it
    // stayed half hidden (a reviewer, 2026-11-14).
    scrollTo (0.0);
    auto top = 0, half = -1, bottom = 0;

    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto height = phazite::ui::getTrackHeight (phazite::getTrackRow (screen.context, tracks[i]).value());

        if (top < screen.lanes->getHeight() && top + height > screen.lanes->getHeight())
        {
            half = (int) i;
            bottom = top + height;
            break;
        }

        top += height;
    }

    REQUIRE (half > 0);
    mouse.click (onHeader (tracks[(size_t) half - 1]));
    phazite::test::deliverChanges();
    REQUIRE (getRowsScroll (screen.playlist) == doctest::Approx (0.0));
    REQUIRE (screen.registry.invoke ("track.selectNext"));
    phazite::test::deliverChanges();
    CHECK (screen.selection().getTracks() == std::vector { tracks[(size_t) half] });
    CHECK (getRowsScroll (screen.playlist) > 0.0);
    CHECK (getRowsScroll (screen.playlist) + screen.lanes->getHeight() >= bottom);
}

TEST_CASE ("A clip dragged onto a track of the other kind, a folder or a return stays on a track it can go on, the hint bar saying "
           "why; a file dropped there goes on a new track below; Draw places the clip selected last, where it suits")
{
    // Audio and notes don't share a track (docs/design/building-blocks.md): an audio clip landed on a track of notes or with an
    // instrument, silenced, and notes on a track of audio, unheard (the audit's ui-timeline-editors 8).
    PlaylistScreen screen;
    const auto audio = screen.addAudioClip (0, 0.0, 1.0); // track 0: audio, beats 0 to 2
    const auto notes = screen.addMidiClip (1, 0.0, 2.0);  // track 1: notes
    const auto folder = phazite::insertFolder (screen.getSong(), screen.tracks[2], nullptr);
    const auto returnTrack = phazite::insertReturnTrack (screen.getSong(), "Reverb", nullptr);
    screen.getSong().getUndoManager().clearUndoHistory();
    screen.selection().clear();
    phazite::test::deliverChanges();

    // The middle of a row's lower half, where the Smart Tool moves a clip: the rows as the model lists them, as tall as they show.
    const auto rowAt = [&screen] (phazite::TrackID track, double beat)
    {
        auto y = 0;

        for (const auto& row : phazite::getPlaylistRows (screen.context))
        {
            if (row.pinned)
                continue;

            const auto height = phazite::ui::getTrackHeight (row);

            if (row.id == track)
                return juce::Point<int> (screen.xOf (beat), y + height * 3 / 4);

            y += height;
        }

        FAIL ("no such row");
        return juce::Point<int>();
    };

    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    const auto dragOnto = [&] (phazite::ClipID clip, phazite::TrackID from, phazite::TrackID onto, const juce::String& why)
    {
        undo.check ("a clip dragged onto a track it can't go on",
                    [&]
                    {
                        const auto start = screen.get (clip).range.getStart().inBeats();
                        mouse.press (rowAt (from, start + 0.5));
                        mouse.dragTo (rowAt (from, start + 1.0));
                        mouse.dragTo (rowAt (onto, start + 2.5));
                        CHECK (screen.get (clip).track == from);
                        CHECK (screen.playlist.getDragReadout().contains (why));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();
        CHECK (screen.get (clip).track == from);
    };

    dragOnto (audio, screen.tracks[0], screen.tracks[1], "An audio clip can't go on a track of notes");
    dragOnto (notes, screen.tracks[1], screen.tracks[0], "A note clip can't go on a track of audio");
    dragOnto (audio, screen.tracks[0], folder, "A folder holds no clips");

    // Copies of both, Ctrl+dragged a row down: the audio's copy would go beside the notes, which stay where they are. Counted
    // as leaving their track, they let it go there (a reviewer, 2026-11-14).
    {
        const auto audioStart = screen.get (audio).range.getStart().inBeats();
        mouse.click (rowAt (screen.tracks[0], audioStart + 0.5));
        mouse.click (rowAt (screen.tracks[1], screen.get (notes).range.getStart().inBeats() + 0.5), ctrl);
        phazite::test::deliverChanges();
        REQUIRE (screen.selection().getClips().size() == 2);

        undo.check ("copies dragged a row down, where the audio's can't go",
                    [&]
                    {
                        mouse.press (rowAt (screen.tracks[0], audioStart + 0.5), ctrl);
                        mouse.dragTo (rowAt (screen.tracks[0], audioStart + 0.5).translated (0, 5)); // down alone: no copy yet
                        mouse.dragTo (rowAt (screen.tracks[1], audioStart + 0.5));
                        CHECK (screen.playlist.getDragReadout().contains ("An audio clip can't go on a track of notes"));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        for (const auto& clip : phazite::getClips (screen.context, screen.tracks[1]))
            CHECK (clip.kind == phazite::ClipType::midi);

        screen.selection().clear();
        phazite::test::deliverChanges();
    }

    // An empty track without an instrument takes either; then it is the audio's.
    undo.check ("an audio clip onto an empty track",
                [&]
                {
                    const auto start = screen.get (audio).range.getStart().inBeats();
                    mouse.press (rowAt (screen.tracks[0], start + 0.5));
                    mouse.dragTo (rowAt (screen.tracks[2], start + 0.5));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();
    CHECK (screen.get (audio).track == screen.tracks[2]);
    dragOnto (notes, screen.tracks[1], screen.tracks[2], "A note clip can't go on a track of audio");

    // The model refuses the same: Draw and Paint place a copy of the clip used last only where it suits.
    CHECK_FALSE (phazite::placeClip (screen.context, screen.tracks[1], beatAt (8.0), audio).isValid());
    CHECK_FALSE (phazite::placeClip (screen.context, returnTrack, beatAt (8.0), notes).isValid());

    // Draw places the clip selected last: the audio, Ctrl+clicked after the notes, which are first in the playlist's order.
    mouse.click (rowAt (screen.tracks[1], screen.get (notes).range.getStart().inBeats() + 0.5));
    mouse.click (rowAt (screen.tracks[2], screen.get (audio).range.getStart().inBeats() + 0.5), ctrl);
    phazite::test::deliverChanges();
    REQUIRE (screen.selection().getClips().size() == 2);
    REQUIRE (screen.selection().getClips().front() == notes);
    screen.playlist.setTool (Tool::draw);
    undo.check ("Draw on the audio's track", [&] { mouse.click (rowAt (screen.tracks[2], 12.0)); });
    phazite::test::deliverChanges();
    const auto onAudioTrack = phazite::getClips (screen.context, screen.tracks[2]);
    REQUIRE (onAudioTrack.size() == 2);
    CHECK (onAudioTrack[1].kind == phazite::ClipType::audio);
    screen.playlist.setTool (Tool::smart);

    // A file of audio dragged over the notes' track or the folder: the hint bar says it goes on a new track below, and it does.
    const auto sound = phazite::test::writeSound (screen.temp.folder.getChildFile ("Dropped.wav"), 1.0);
    const auto overRow = [&] (phazite::TrackID track) { return screen.playlist.getLocalPoint (screen.lanes, rowAt (track, 12.0)); };
    auto& dropTarget = static_cast<juce::FileDragAndDropTarget&> (screen.playlist); // as Explorer reaches it

    for (const auto track : { screen.tracks[1], folder })
    {
        const auto at = overRow (track);
        const auto tracksBefore = phazite::getAllPlaylistTracks (screen.context).size();
        dropTarget.fileDragMove ({ sound.getFullPathName() }, at.x, at.y);
        CHECK (screen.playlist.getDragReadout().startsWith ("Goes on a new track below"));
        undo.check ("a file dropped on a track it doesn't suit",
                    [&]
                    {
                        dropTarget.filesDropped ({ sound.getFullPathName() }, at.x, at.y);
                        phazite::test::waitForAudioWork (screen.context); // read in the background (known bug 123)
                    });
        phazite::test::deliverChanges();
        CHECK (screen.playlist.getDragReadout().isEmpty());
        CHECK (phazite::getAllPlaylistTracks (screen.context).size() == tracksBefore + 1);
        CHECK (phazite::getClips (screen.context, track).size() == (track == folder ? 0u : 1u));
    }

    // A return, which the playlist doesn't show, takes none either: a file dropped on it went on it, unheard.
    CHECK_FALSE (phazite::importsOnto (screen.context, sound, returnTrack));
    undo.check ("a file put on a return",
                [&] { phazite::importFiles (screen.context, { sound.getFullPathName() }, returnTrack, beatAt (0.0)); });
    CHECK (phazite::getClips (screen.context, returnTrack).empty());

    undo.walkBackAndForth();
}

TEST_CASE ("In an automation lane, a double-click on empty space adds a point, one undo step; Shift in the middle of a point's drag "
           "makes what follows finer, without a jump (known bug 56)")
{
    PlaylistScreen screen;
    const auto volume = phazite::getVolume (screen.context, screen.tracks[0]);
    phazite::showAutomationLane (phazite::getVolume (screen.context, screen.tracks[0]));
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    auto* lane = phazite::test::findComponentWithTitle (screen.playlist, "Automation of Volume");
    REQUIRE (lane != nullptr);
    const auto inLane = [&] (double beat, float value)
    {
        const auto area =
            lane->getLocalBounds().toFloat().withTrimmedBottom (1.0f).reduced (0.0f, phazite::ui::LaneTint::selectedPointRadius + 1.0f);
        return juce::Point<int> (screen.xOf (beat), juce::roundToInt (area.getBottom() - value * area.getHeight()));
    };
    const auto points = [&] { return phazite::getAutomationPoints (screen.context, volume.getRef()); };
    Mouse mouse (*lane);
    UndoCheck undo (screen.getSong());

    // The click added a point, and the double-click it began deleted it: two undo steps for nothing.
    undo.check ("a double-click on empty space", [&] { mouse.doubleClick (inLane (4.0, 0.5f)); });
    phazite::test::deliverChanges();
    REQUIRE (points().size() == 1);
    CHECK (points()[0].beat.inBeats() == doctest::Approx (4.0));
    CHECK (screen.describeUndo() == "Undo Add Automation Point");

    // A double-click on a point still deletes it.
    undo.check ("a double-click on the point", [&] { mouse.doubleClick (inLane (4.0, 0.5f)); });
    phazite::test::deliverChanges();
    CHECK (points().empty());

    // Shift pressed in the middle of a drag: the value went where the whole drag, a fifth as far, put it.
    phazite::addAutomationPoint (screen.context, volume.getRef(), { beatAt (8.0), 0.5f });
    phazite::test::deliverChanges();
    const auto from = inLane (8.0, 0.5f);
    const auto valueOf = [&] { return points().at (0).value; };
    const auto step = std::max (4, lane->getHeight() / 10); // pixels, past the drag's threshold, the value well inside its range
    undo.check ("a point dragged, Shift pressed in the middle",
                [&]
                {
                    mouse.press (from);
                    mouse.dragTo (from.translated (0, -step));
                    mouse.dragTo (from.translated (0, -2 * step));
                    const auto beforeShift = valueOf();
                    CHECK (beforeShift > 0.5f);

                    // Shift goes down, the pointer where it was: the value stays; then it moves a fifth as far.
                    mouse.holdKeys (juce::ModifierKeys::shiftModifier);
                    mouse.dragTo (from.translated (0, -2 * step));
                    CHECK (valueOf() == doctest::Approx (beforeShift));
                    mouse.dragTo (from.translated (0, -4 * step));
                    const auto fine = valueOf();
                    CHECK (fine > beforeShift);
                    CHECK (fine - beforeShift < (beforeShift - 0.5f) / 2.0f); // as far again, a fifth as far: a fifth of it
                    mouse.release();
                    mouse.pause();
                });
    undo.walkBackAndForth();
}

//==============================================================================
TEST_CASE ("The playlist's Slice tool splits a clip on the grid where it is clicked, off it with Alt, each click one undo step")
{
    PlaylistScreen screen;
    screen.addMidiClip (0, 0.5, 6.0); // off the grid: from beat 0.5 to 6.5
    phazite::test::dispatchPendingMessages();
    screen.playlist.setTool (Tool::slice);
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    undo.check ("a click on the clip", [&] { mouse.click (screen.at (3.2, 0)); });
    phazite::test::deliverChanges();
    CHECK (screen.describeUndo() == "Undo Split Clip");
    CHECK (screen.startsOn (0) == std::vector { 0.5, 3.0 });
    phazite::test::deliverChanges();

    // With Alt, where the pointer is.
    undo.check ("an Alt+click", [&] { mouse.click (screen.at (5.3, 0), alt); });
    phazite::test::deliverChanges();
    const auto starts = screen.startsOn (0);
    REQUIRE (starts.size() == 3);
    CHECK (std::abs (starts[2] - 5.3) <= pixelInBeats (screen.playlist));

    // Empty space: nothing, and no step.
    undo.check ("a click on empty space", [&] { mouse.click (screen.at (20.0, 0)); });
    phazite::test::deliverChanges();
    CHECK (undo.getNumSteps() == 2);
    undo.walkBackAndForth();
}

TEST_CASE ("The playlist's Slip tool moves the audio inside a clip by the grid's steps, the clip staying where it is")
{
    PlaylistScreen screen;
    const auto clip = screen.addAudioClip (2, 2.0);            // 4 seconds: beats 2 to 10
    phazite::trimClipEnd (screen.context, clip, beatAt (8.0)); // its file's first 3 seconds: a slip keeps it in the file
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    screen.playlist.setTool (Tool::slip);
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());
    const auto rangeBefore = screen.get (clip).range;

    // A beat and a third to the left: the audio a whole beat earlier, so the clip plays from half a second later in its file.
    undo.check ("a slip",
                [&]
                {
                    mouse.press (screen.at (6.0, 2));
                    mouse.dragTo (screen.at (5.5, 2));
                    mouse.dragTo (screen.at (6.0 - 1.3, 2));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Slip Clip");
    CHECK (phazite::getSlipOffset (screen.context, clip) == doctest::Approx (0.5));
    CHECK (screen.get (clip).range == rangeBefore);
    undo.walkBackAndForth();
}

TEST_CASE ("The playlist's Mute and Delete tools act once on each clip the drag passes over, as one undo step")
{
    for (const auto tool : { Tool::mute, Tool::erase })
    {
        CAPTURE (phazite::ui::getToolName (tool).toStdString());
        PlaylistScreen screen;
        const auto a = screen.addMidiClip (0, 0.0, 2.0), b = screen.addMidiClip (0, 4.0, 2.0), c = screen.addMidiClip (1, 0.0, 2.0);
        const auto untouched = screen.addMidiClip (2, 0.0, 2.0);
        phazite::test::dispatchPendingMessages();
        screen.getSong().getUndoManager().clearUndoHistory();
        screen.playlist.setTool (tool);
        Mouse mouse (*screen.lanes);
        UndoCheck undo (screen.getSong());

        // Over a, on to b, back over a (which it leaves as it is now), and down to c.
        undo.check ("a drag over three clips",
                    [&]
                    {
                        mouse.press (screen.at (1.0, 0));
                        mouse.dragTo (screen.at (3.0, 0));
                        mouse.dragTo (screen.at (5.0, 0));
                        mouse.dragTo (screen.at (1.5, 0));
                        mouse.dragTo (screen.at (1.0, 1));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        if (tool == Tool::mute)
        {
            CHECK (screen.describeUndo() == "Undo Mute Clips");
            CHECK (screen.get (a).muted);
            CHECK (screen.get (b).muted);
            CHECK (screen.get (c).muted);
            CHECK_FALSE (screen.get (untouched).muted);
        }
        else
        {
            CHECK (screen.describeUndo() == "Undo Delete Clips");
            CHECK_FALSE (screen.has (a));
            CHECK_FALSE (screen.has (b));
            CHECK_FALSE (screen.has (c));
            CHECK (screen.has (untouched));
        }

        undo.walkBackAndForth();
    }
}

TEST_CASE ("The playlist's Select tool selects the clips in a rectangle, Ctrl adds to them, and a click on empty space clears them")
{
    PlaylistScreen screen;
    const auto a = screen.addMidiClip (0, 0.0, 2.0), b = screen.addMidiClip (1, 4.0, 2.0), c = screen.addMidiClip (2, 8.0, 2.0);
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    screen.playlist.setTool (Tool::select);
    auto& selection = screen.context.getSongSelection();
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    undo.check ("a rectangle",
                [&]
                {
                    mouse.press (screen.at (1.0, 0));
                    mouse.dragTo (screen.at (3.0, 0));
                    mouse.dragTo (screen.at (5.0, 1));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (selection.getClips() == std::vector { a, b });
    phazite::test::deliverChanges();

    undo.check ("a rectangle with Ctrl",
                [&]
                {
                    mouse.press (screen.at (7.0, 2), ctrl);
                    mouse.dragTo (screen.at (9.0, 2).translated (0, 4));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (selection.getClips() == std::vector { a, b, c });
    phazite::test::deliverChanges();

    undo.check ("a click on empty space", [&] { mouse.click (screen.at (20.0, 1)); });
    phazite::test::deliverChanges();
    CHECK (selection.getClips().empty());
    CHECK (undo.getNumSteps() == 0); // selecting is no change to the song
}

TEST_CASE ("The playlist's Zoom tool zooms in at a click, out with Alt, and to the time a drag covers")
{
    PlaylistScreen screen;
    screen.playlist.setTool (Tool::zoom);
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());
    const auto& view = screen.playlist.getView();
    const auto zoomBefore = view.pixelsPerBeat;

    undo.check ("a click", [&] { mouse.click (screen.at (8.0, 1)); });
    phazite::test::deliverChanges();
    CHECK (view.pixelsPerBeat == doctest::Approx (zoomBefore * 2.0));

    undo.check ("an Alt+click", [&] { mouse.click (screen.at (8.0, 1), alt); });
    phazite::test::deliverChanges();
    CHECK (view.pixelsPerBeat == doctest::Approx (zoomBefore));

    // From beat 4 to beat 8: those four beats fill the lanes.
    undo.check ("a drag",
                [&]
                {
                    mouse.press (screen.at (4.0, 0));
                    mouse.dragTo (screen.at (6.0, 1));
                    mouse.dragTo (screen.at (8.0, 2));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (view.start.inBeats() == doctest::Approx (4.0).epsilon (0.05));
    CHECK (view.xToBeat ((double) screen.lanes->getParentComponent()->getWidth()).inBeats() == doctest::Approx (8.0).epsilon (0.05));
    CHECK (undo.getNumSteps() == 0);
}

TEST_CASE ("The Smart Tool trims a clip's start to the grid from its left edge, and an off-grid clip's end to the grid")
{
    PlaylistScreen screen;
    const auto clip = screen.addMidiClip (0, 2.5, 7.0); // from 2.5 to 9.5: both ends off the grid
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    undo.check ("its start dragged in",
                [&]
                {
                    const auto edge = screen.boundsOf (clip);
                    const juce::Point<int> grip (edge.getX() + 2, edge.getBottom() - 4);
                    mouse.press (grip);
                    mouse.dragTo (grip.translated (5, 0));
                    mouse.dragTo ({ screen.xOf (4.2), grip.y });
                    phazite::test::deliverChanges();
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Trim Clip Start");
    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (screen.get (clip).range.getEnd().inBeats() == doctest::Approx (9.5));

    undo.check ("its end dragged out",
                [&]
                {
                    const auto edge = screen.boundsOf (clip);
                    const juce::Point<int> grip (edge.getRight() - 2, edge.getBottom() - 4);
                    mouse.press (grip);
                    mouse.dragTo (grip.translated (5, 0));
                    mouse.dragTo ({ screen.xOf (11.8), grip.y });
                    phazite::test::deliverChanges();
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Trim Clip End");
    CHECK (screen.get (clip).range.getEnd().inBeats() == doctest::Approx (12.0));
    undo.walkBackAndForth();
}

TEST_CASE ("The Smart Tool fades an audio clip from its top corners, and crossfades two where they meet")
{
    // Two clips of 8 beats' audio that meet at beat 6, each with audio to spare past it: the first from 0 to 6, the second
    // from 6 to 12, its audio from beat 4.
    PlaylistScreen screen;
    const auto first = screen.addAudioClip (2, 0.0), second = screen.addAudioClip (2, 10.0);
    phazite::trimClipEnd (screen.context, first, beatAt (6.0));
    phazite::trimClipStart (screen.context, second, beatAt (12.0));
    phazite::moveClip (screen.context, second, beatAt (6.0), screen.tracks[2]);
    REQUIRE (screen.get (second).range == phazite::BeatRange (beatAt (6.0), beatAt (12.0)));
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    undo.check ("a fade in",
                [&]
                {
                    const auto bounds = screen.boundsOf (first);
                    const auto corner = bounds.getTopLeft().translated (3, 3);
                    mouse.press (corner);
                    mouse.dragTo (corner.translated (6, 0));
                    mouse.dragTo ({ screen.xOf (2.0), corner.y });
                    phazite::test::deliverChanges();
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Fade In");
    CHECK (screen.get (first).fadeIn == doctest::Approx (2.0 / 6.0).epsilon (0.02));

    undo.check ("a fade out",
                [&]
                {
                    const auto bounds = screen.boundsOf (second);
                    const auto corner = bounds.getTopRight().translated (-3, 3);
                    mouse.press (corner);
                    mouse.dragTo (corner.translated (-6, 0));
                    mouse.dragTo ({ screen.xOf (10.0), corner.y });
                    phazite::test::deliverChanges();
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Fade Out");
    CHECK (screen.get (second).fadeOut == doctest::Approx (2.0 / 6.0).epsilon (0.02));

    // Where they meet, low on them: a drag a beat on crossfades them over two beats, each playing over the other.
    undo.check ("a crossfade",
                [&]
                {
                    const auto bounds = screen.boundsOf (first);
                    const juce::Point<int> meeting (screen.xOf (6.0), bounds.getBottom() - 3);
                    mouse.press (meeting);
                    mouse.dragTo (meeting.translated (6, 0));
                    mouse.dragTo ({ screen.xOf (7.0), meeting.y });
                    phazite::test::deliverChanges();
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Crossfade");
    CHECK (screen.get (first).range.getEnd().inBeats() == doctest::Approx (7.0).epsilon (0.02));
    CHECK (screen.get (second).range.getStart().inBeats() == doctest::Approx (5.0).epsilon (0.02));
    CHECK (screen.get (first).fadeOut > 0.0);
    CHECK (screen.get (second).fadeIn > 0.0);
    undo.walkBackAndForth();
}

TEST_CASE ("Ctrl and Shift and a drag make an alias of a clip, linked to it, as one undo step")
{
    PlaylistScreen screen;
    const auto clip = screen.addMidiClip (0, 0.0, 4.0);
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    undo.check ("an alias dragged",
                [&]
                {
                    mouse.press (screen.lower (1.0, 0),
                                 juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier));
                    mouse.dragTo (screen.lower (3.0, 0));
                    mouse.dragTo (screen.lower (5.0, 0));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Make Alias");
    CHECK (screen.startsOn (0) == std::vector { 0.0, 4.0 });
    phazite::test::deliverChanges();
    const auto linked = phazite::getLinkedClips (screen.context, clip);
    CHECK (linked.size() == 2);
    CHECK (screen.get (clip).linked);
    undo.walkBackAndForth();
}

TEST_CASE ("A clip off the grid dragged to another track stays at its place in time, and dragged along keeps its offset")
{
    PlaylistScreen screen;
    const auto clip = screen.addMidiClip (0, 1.3, 4.0);
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    undo.check ("down a track",
                [&]
                {
                    mouse.press (screen.lower (3.0, 0));
                    mouse.dragTo (screen.lower (3.1, 0).translated (0, 5));
                    mouse.dragTo (screen.lower (3.1, 1));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Move Clip");
    CHECK (screen.get (clip).track == screen.tracks[1]);
    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (1.3));

    undo.check ("two beats and a bit along",
                [&]
                {
                    mouse.press (screen.lower (3.0, 1));
                    mouse.dragTo (screen.lower (4.0, 1));
                    mouse.dragTo (screen.lower (5.2, 1));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (3.3));
    undo.walkBackAndForth();
}

TEST_CASE ("A clip dragged out of the playlist's view and back lands where the pointer has it in the view, as one undo step")
{
    PlaylistScreen screen;
    const auto clip = screen.addMidiClip (0, 2.0, 4.0);
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());
    const auto press = screen.lower (3.0, 0);
    const auto pressBeat = screen.playlist.getView().xToBeat ((double) press.x).inBeats();

    undo.check ("out past the right edge and back",
                [&]
                {
                    mouse.press (press);
                    mouse.dragTo (press.translated (10, 0));

                    for (int i = 0; i < 20; ++i)
                        mouse.dragTo ({ screen.lanes->getParentComponent()->getWidth() + 40 + i % 2, press.y });
                    phazite::test::deliverChanges();

                    REQUIRE (screen.playlist.getView().start.inBeats() > 0.0); // it scrolled
                    mouse.dragTo (press);
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    // The view stays scrolled; the pointer let go where the press was on the screen, which is later in the song now: the clip
    // went as far, by whole beats.
    const auto& view = screen.playlist.getView();
    const auto expected = 2.0 + std::round (view.xToBeat ((double) press.x).inBeats() - pressBeat);
    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (expected));
    CHECK (screen.describeUndo() == "Undo Move Clip");
    CHECK (screen.playlist.getDragReadout().isEmpty());
    undo.walkBackAndForth();
}

TEST_CASE ("A drag let go outside the playlist's window ends there, as one undo step, and the next click is a click")
{
    PlaylistScreen screen;
    const auto clip = screen.addMidiClip (0, 4.0, 4.0);
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    // Left, out of the window: the clip goes as far as the song's start, no further.
    undo.check ("a drag let go outside",
                [&]
                {
                    mouse.press (screen.lower (5.0, 0));
                    mouse.dragTo (screen.lower (4.0, 0));
                    mouse.dragTo (leftOfWindow (screen.window, *screen.lanes, screen.lower (4.0, 0)));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (screen.describeUndo() == "Undo Move Clip");
    CHECK (screen.playlist.getDragReadout().isEmpty());

    // The drag is over: a click on empty space selects its track, nothing more.
    undo.check ("a click", [&] { mouse.click (screen.at (20.0, 1)); });
    phazite::test::deliverChanges();
    CHECK (screen.get (clip).range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (undo.getNumSteps() == 1);
    undo.walkBackAndForth();
}

TEST_CASE ("A tool's key pressed in the middle of a drag changes the tool once the mouse goes up, the drag going on as it began")
{
    PlaylistScreen screen;
    const auto a = screen.addMidiClip (0, 0.0, 2.0), b = screen.addMidiClip (0, 4.0, 2.0);
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    screen.playlist.setTool (Tool::erase);
    Mouse mouse (*screen.lanes);
    UndoCheck undo (screen.getSong());

    undo.check ("a Delete drag with the Mute tool's key pressed in it",
                [&]
                {
                    mouse.press (screen.at (1.0, 0));
                    mouse.dragTo (screen.at (2.5, 0));
                    screen.window.pressKey (getKey ("T")); // the Mute tool's
                    screen.window.releaseKeys();
                    CHECK (screen.playlist.getTool() == Tool::erase);
                    mouse.dragTo (screen.at (5.0, 0));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK_FALSE (screen.has (a));
    CHECK_FALSE (screen.has (b));
    CHECK (screen.describeUndo() == "Undo Delete Clips");
    CHECK (screen.playlist.getTool() == Tool::mute);
    undo.walkBackAndForth();
}

//==============================================================================
TEST_CASE ("The piano roll's Slice tool splits a note on the grid where it is clicked, off it with Alt")
{
    PianoRollScreen screen;
    const auto note = screen.addNote (1.5, 0, 3.0); // off the grid: from beat 1.5 to 4.5
    screen.getSong().getUndoManager().clearUndoHistory();
    screen.pianoRoll.setTool (Tool::slice);
    Mouse mouse (*screen.notes);
    UndoCheck undo (screen.getSong());
    const auto y = screen.middleOf (note).y;

    undo.check ("a click on the note", [&] { mouse.click ({ screen.xOf (3.2), y }); });
    phazite::test::deliverChanges();
    CHECK (screen.describeUndo() == "Undo Split Note");
    REQUIRE (screen.getAll().size() == 2);
    CHECK (screen.get (note).start.inBeats() == doctest::Approx (1.5)); // the left part keeps its ID
    CHECK (screen.get (note).length.inBeats() == doctest::Approx (1.5));

    undo.check ("an Alt+click", [&] { mouse.click ({ screen.xOf (2.3), y }, alt); });
    phazite::test::deliverChanges();
    REQUIRE (screen.getAll().size() == 3);
    CHECK (std::abs (screen.get (note).length.inBeats() - 0.8) <= pixelInBeats (screen.pianoRoll));

    undo.check ("a click on empty grid", [&] { mouse.click ({ screen.xOf (6.5), y }); });
    phazite::test::deliverChanges();
    CHECK (screen.getAll().size() == 3);
    CHECK (undo.getNumSteps() == 2);
    undo.walkBackAndForth();
}

TEST_CASE ("The piano roll's Mute and Delete tools act once on each note the drag passes over, as one undo step")
{
    for (const auto tool : { Tool::mute, Tool::erase })
    {
        CAPTURE (phazite::ui::getToolName (tool).toStdString());
        PianoRollScreen screen;
        const auto a = screen.addNote (1.0), b = screen.addNote (3.0), c = screen.addNote (3.0, 2), untouched = screen.addNote (6.0, 4);
        screen.getSong().getUndoManager().clearUndoHistory();
        screen.pianoRoll.setTool (tool);
        Mouse mouse (*screen.notes);
        UndoCheck undo (screen.getSong());

        const auto onA = screen.middleOf (a), onB = screen.middleOf (b), onC = screen.middleOf (c);

        undo.check ("a drag over three notes",
                    [&]
                    {
                        mouse.press (onA);
                        mouse.dragTo (onB);
                        mouse.dragTo (onA);
                        mouse.dragTo (onC);
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        if (tool == Tool::mute)
        {
            CHECK (screen.describeUndo() == "Undo Mute Notes");
            CHECK (screen.get (a).muted);
            CHECK (screen.get (b).muted);
            CHECK (screen.get (c).muted);
            CHECK_FALSE (screen.get (untouched).muted);
        }
        else
        {
            CHECK (screen.describeUndo() == "Undo Delete Notes");
            CHECK_FALSE (screen.has (a));
            CHECK_FALSE (screen.has (b));
            CHECK_FALSE (screen.has (c));
            CHECK (screen.has (untouched));
        }

        undo.walkBackAndForth();
    }
}

TEST_CASE ("The piano roll's Select tool selects the notes in a rectangle, Ctrl adds to them, and a click on empty grid clears them")
{
    PianoRollScreen screen;
    const auto a = screen.addNote (1.0), b = screen.addNote (3.0, 2), c = screen.addNote (6.0, -3);
    screen.getSong().getUndoManager().clearUndoHistory();
    screen.pianoRoll.setTool (Tool::select);
    Mouse mouse (*screen.notes);
    UndoCheck undo (screen.getSong());
    const auto selected = [&screen]
    {
        auto notes = screen.getSelected();
        std::sort (notes.begin(), notes.end()); // in the order they were made
        return notes;
    };

    undo.check ("a rectangle",
                [&]
                {
                    mouse.press (screen.middleOf (a).translated (0, screen.rowHeight()));
                    mouse.dragTo (screen.middleOf (a));
                    mouse.dragTo (screen.middleOf (b).translated (0, -screen.rowHeight()));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (selected() == std::vector { a, b });
    phazite::test::deliverChanges();

    undo.check ("a rectangle with Ctrl",
                [&]
                {
                    mouse.press (screen.middleOf (c).translated (-screen.widthOf (0.25), 0), ctrl);
                    mouse.dragTo (screen.middleOf (c).translated (screen.widthOf (0.25), 2));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (selected() == std::vector { a, b, c });
    phazite::test::deliverChanges();

    undo.check ("a click on empty grid", [&] { mouse.click ({ screen.xOf (7.5), screen.yOf (screen.middlePitch() + 6) }); });
    phazite::test::deliverChanges();
    CHECK (screen.getSelected().empty());
    CHECK (undo.getNumSteps() == 0);
}

TEST_CASE ("The piano roll's Zoom tool zooms in at a click, out with Alt, and to the time a drag covers")
{
    PianoRollScreen screen;
    screen.pianoRoll.setTool (Tool::zoom);
    Mouse mouse (*screen.notes);
    UndoCheck undo (screen.getSong());
    const auto& view = screen.pianoRoll.getView();
    const auto zoomBefore = view.pixelsPerBeat;

    undo.check ("a click", [&] { mouse.click (screen.at (4.0)); });
    phazite::test::deliverChanges();
    CHECK (view.pixelsPerBeat == doctest::Approx (zoomBefore * 2.0));

    undo.check ("an Alt+click", [&] { mouse.click (screen.at (4.0), alt); });
    phazite::test::deliverChanges();
    CHECK (view.pixelsPerBeat == doctest::Approx (zoomBefore));

    undo.check ("a drag",
                [&]
                {
                    mouse.press (screen.at (2.0));
                    mouse.dragTo (screen.at (3.0));
                    mouse.dragTo (screen.at (4.0));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (view.start.inBeats() == doctest::Approx (2.0).epsilon (0.05));
    CHECK (view.xToBeat ((double) screen.notes->getParentComponent()->getWidth()).inBeats() == doctest::Approx (4.0).epsilon (0.05));
    CHECK (undo.getNumSteps() == 0);
    CHECK (screen.getAll().empty());
}

TEST_CASE ("A note dragged out of the piano roll's view and back lands where the pointer has it, and one let go outside the window "
           "ends there")
{
    PianoRollScreen screen;
    const auto note = screen.addNote (2.0);
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.notes);
    UndoCheck undo (screen.getSong());

    // Up past the top, where the rows scroll, and back down to where the press was: as many rows higher as they scrolled.
    const auto press = screen.middleOf (note);
    const auto pitchBefore = screen.get (note).pitch;

    undo.check ("out past the top and back",
                [&]
                {
                    mouse.press (press);
                    mouse.dragTo (press.translated (0, -screen.rowHeight()));

                    for (int i = 0; i < 20; ++i)
                        mouse.dragTo ({ press.x, -30 - i % 2 });
                    phazite::test::deliverChanges();

                    mouse.dragTo (press);
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.describeUndo() == "Undo Move Notes");
    CHECK (screen.get (note).pitch == screen.pitchAt (press.y));
    CHECK (screen.get (note).pitch > pitchBefore);
    CHECK (screen.get (note).start.inBeats() == doctest::Approx (2.0));

    // Left, out of the window: as far as its clip's start.
    undo.check ("a drag let go outside",
                [&]
                {
                    const auto from = screen.middleOf (note);
                    mouse.press (from);
                    mouse.dragTo (from.translated (-10, 0));
                    mouse.dragTo (leftOfWindow (screen.window, *screen.notes, from));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK (screen.get (note).start.inBeats() == doctest::Approx (0.0));
    CHECK (screen.pianoRoll.getDragReadout().isEmpty());
    undo.walkBackAndForth();
}

TEST_CASE ("A note dragged past its clip's end and back leaves the clip as it was; let go past it, the clip grows to hold it")
{
    // The clip grew at each step of the drag, and never shrank again: let go back inside, it stayed a bar longer, and the
    // song's end with it (known bug 44).
    PianoRollScreen screen;
    screen.pianoRoll.zoomTo ({ beatAt (0.0), beatAt (16.0) });
    phazite::test::deliverChanges();
    const auto note = screen.addNote (6.0);
    screen.getSong().getUndoManager().clearUndoHistory();
    Mouse mouse (*screen.notes);
    UndoCheck undo (screen.getSong());
    const auto clipEnd = [&screen] { return phazite::getClip (screen.context, screen.clip).value().range.getEnd().inBeats(); };
    REQUIRE (clipEnd() == doctest::Approx (8.0));

    SUBCASE ("Moved")
    {
        undo.check ("past the end and back",
                    [&]
                    {
                        const auto from = screen.middleOf (note);
                        mouse.press (from);
                        mouse.dragTo (from.translated (screen.widthOf (4.0), 0));
                        phazite::test::deliverChanges();
                        CHECK (clipEnd() == doctest::Approx (12.0)); // to the end of the note's bar, while it is there
                        mouse.dragTo (from.translated (-screen.widthOf (3.0), 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.get (note).start.inBeats() == doctest::Approx (3.0));
        CHECK (clipEnd() == doctest::Approx (8.0));

        // Let go past it, the clip holds it.
        undo.check ("past the end",
                    [&]
                    {
                        const auto from = screen.middleOf (note);
                        mouse.press (from);
                        mouse.dragTo (from.translated (screen.widthOf (6.0), 0));
                        mouse.release();
                        mouse.pause();
                    });
        CHECK (screen.get (note).start.inBeats() == doctest::Approx (9.0));
        CHECK (clipEnd() == doctest::Approx (12.0));
    }

    SUBCASE ("Copied")
    {
        undo.check ("a copy past the end and back",
                    [&]
                    {
                        const auto from = screen.middleOf (note);
                        mouse.press (from, ctrl);
                        mouse.dragTo (from.translated (screen.widthOf (4.0), 0));
                        phazite::test::deliverChanges();
                        mouse.dragTo (from.translated (-screen.widthOf (2.0), 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.getAll().size() == 2);
        CHECK (clipEnd() == doctest::Approx (8.0));
    }

    SUBCASE ("Made longer")
    {
        undo.check ("its end past the clip's and back",
                    [&]
                    {
                        const auto end = juce::Point<int> (screen.xOf (7.0) - 2, screen.yOf (screen.get (note).pitch));
                        mouse.press (end);
                        mouse.dragTo (end.translated (screen.widthOf (4.0), 0));
                        phazite::test::deliverChanges();
                        mouse.dragTo (end.translated (screen.widthOf (1.0), 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.get (note).length.inBeats() == doctest::Approx (2.0)); // to the clip's end
        CHECK (clipEnd() == doctest::Approx (8.0));
    }

    SUBCASE ("Added by the click past the clip's end, and dragged back in the same press")
    {
        // A note two beats long is the next note (the note pressed last), so a click at beat 7 adds one past the clip's end,
        // which grows the clip. The ends were taken as the drag began, after the click had grown the clip: dragged back, the
        // clip stayed 12 beats long (a reviewer, 2026-11-14).
        const auto twoBeats = screen.addNote (0.0, 2, 2.0);
        mouse.click (screen.middleOf (twoBeats));
        phazite::test::deliverChanges();
        REQUIRE (screen.pianoRoll.getNextNoteLength().inBeats() == doctest::Approx (2.0));
        const juce::Point<int> at (screen.xOf (7.0) + 2, screen.yOf (screen.middlePitch() - 3));

        undo.check ("a note added past the end, dragged back",
                    [&]
                    {
                        mouse.press (at);
                        phazite::test::deliverChanges();
                        CHECK (clipEnd() == doctest::Approx (12.0));
                        mouse.dragTo (at.translated (-screen.widthOf (5.0), 0));
                        mouse.release();
                        mouse.pause();
                    });
        phazite::test::deliverChanges();

        CHECK (screen.getAll().size() == 3);
        CHECK (clipEnd() == doctest::Approx (8.0));
        CHECK (screen.describeUndo() == "Undo Add Note");

        // Let go past the end, it keeps the growth; and Escape takes it all back, the note and the growth.
        undo.check ("a note added past the end, dragged on",
                    [&]
                    {
                        mouse.press (at.translated (0, screen.rowHeight() * 2));
                        mouse.dragTo (at.translated (screen.widthOf (1.0), screen.rowHeight() * 2));
                        mouse.release();
                        mouse.pause();
                    });
        CHECK (clipEnd() == doctest::Approx (12.0));

        const auto steps = undo.getNumSteps();
        mouse.press (at.translated (0, screen.rowHeight() * 4));
        mouse.dragTo (at.translated (-screen.widthOf (5.0), screen.rowHeight() * 4));
        screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
        mouse.release();
        mouse.pause();
        phazite::test::deliverChanges();
        CHECK (screen.getAll().size() == 4);
        CHECK (clipEnd() == doctest::Approx (12.0));
        CHECK (undo.getNumSteps() == steps);
    }

    undo.walkBackAndForth();
}

TEST_CASE ("In the piano roll, a tool's key pressed in the middle of a drag changes the tool once the mouse goes up")
{
    PianoRollScreen screen;
    const auto a = screen.addNote (1.0), b = screen.addNote (3.0);
    screen.getSong().getUndoManager().clearUndoHistory();
    screen.pianoRoll.setTool (Tool::erase);
    Mouse mouse (*screen.notes);
    UndoCheck undo (screen.getSong());

    undo.check ("a Delete drag with the Mute tool's key pressed in it",
                [&]
                {
                    mouse.press (screen.middleOf (a));
                    screen.window.pressKey (getKey ("T"));
                    screen.window.releaseKeys();
                    CHECK (screen.pianoRoll.getTool() == Tool::erase);
                    mouse.dragTo (screen.middleOf (b));
                    mouse.release();
                    mouse.pause();
                });
    phazite::test::deliverChanges();

    CHECK_FALSE (screen.has (a));
    CHECK_FALSE (screen.has (b));
    CHECK (screen.describeUndo() == "Undo Delete Notes");
    CHECK (screen.pianoRoll.getTool() == Tool::mute);
    undo.walkBackAndForth();
}
