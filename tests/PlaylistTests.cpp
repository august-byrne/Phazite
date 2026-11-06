// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist, driven with the mouse and the keys as the user would.
#include "UiTestHelpers.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "ui/components/DisplayTicker.h"
#include "ui/components/TrackSwitch.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/playlist/TrackHeader.h"
#include "ui/playlist/TrackHeights.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

// A playlist in a window, with three tracks and the commands and keys the app gives it. The mouse uses the tools
// in its lanes.
struct PlaylistScreen
{
    PlaylistScreen()
    {
        tracks = phazite::ensureTracks (*song, 3);
        song->getUndoManager().clearUndoHistory();

        phazite::addTrackCommands (registry, selection); // Collapse All's
        phazite::ui::addTimelineCommands (registry, [this] { return &playlist; });
        playlist.addCommands (registry);
        setSong (context, song.get());
        window.addKeyListener (&shortcutKeys); // as the main window does
        phazite::test::dispatchPendingMessages();

        lanes = phazite::test::findComponentWithTitle (playlist, "Tracks");
        REQUIRE (lanes != nullptr);
    }

    ~PlaylistScreen()
    {
        window.removeKeyListener (&shortcutKeys);
        setSong (context, nullptr);
    }

    // How tall a track shows, through the model's row of it.
    int heightOf (int track) const
    {
        return phazite::ui::getTrackHeight (phazite::getTrackRow (context, tracks[(size_t) track]).value_or (phazite::TrackRow()));
    }

    // Where a beat is on a track's row, in the lanes (the middle of the row).
    juce::Point<int> at (double beat, int track) const
    {
        auto y = 0;

        for (int i = 0; i < track; ++i)
            y += heightOf (i);

        return { juce::roundToInt (playlist.getView().beatToX (beats (beat))), y + heightOf (track) / 2 };
    }

    phazite::Song& getSong() const { return *song; }

    double startOf (phazite::ClipID clip) const { return phazite::getClipRange (getSong(), clip).getStart().inBeats(); }

    std::vector<double> getClipStarts (int track) const
    {
        std::vector<double> starts;

        for (const auto clip : phazite::getClips (getSong(), tracks[(size_t) track]))
            starts.push_back (startOf (clip));

        std::sort (starts.begin(), starts.end());
        return starts;
    }

    phazite::ClipID addClip (int track, double start, double length)
    {
        const auto clip = phazite::insertMidiClip (getSong(), tracks[(size_t) track], { beats (start), beats (start + length) }, nullptr);
        REQUIRE (clip.isValid());
        return clip;
    }

    // Where a clip shows, in the lanes; nothing while it has no component (out of view).
    juce::Rectangle<int> boundsOf (phazite::ClipID clip)
    {
        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (playlist))
            if (component->getClip() == clip)
                return lanes->getLocalArea (component, component->getLocalBounds());

        return {};
    }

    phazite::Song::Ptr song = phazite::createSong(); // 120 BPM, 4/4
    std::vector<phazite::TrackID> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    phazite::Selection& selection = getSelection (context);
    phazite::RangeSelection& range = getRangeSelection (context);
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
    const auto clip = screen.addClip (0, 0.0, 4.0);
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
    CHECK (screen.startOf (clip) > widthInBeats);
}

TEST_CASE ("A clip dragged along its track takes the automation under it, Ctrl copies it too, and to another track it stays")
{
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 4.0, 4.0);
    const auto volume = phazite::getVolume (screen.context, screen.tracks[0]).getRef();

    // A peak at beat 6, under the clip.
    for (const auto& [beat, value] :
         { std::pair (0.0, 0.5f), std::pair (5.0, 0.5f), std::pair (6.0, 0.9f), std::pair (7.0, 0.5f), std::pair (16.0, 0.5f) })
        phazite::addAutomationPoint (screen.context, volume, { phazite::BeatPosition::fromBeats (beat), value });

    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();

    const auto valueAt = [&screen, &volume] (double beat)
    {
        const auto at = phazite::BeatPosition::fromBeats (beat);
        return phazite::getAutomationValues (screen.context, volume, { at, at }, 1).front();
    };

    // The lower half of a clip moves it. The first track's automation shows its lane under it, above the second track.
    phazite::test::Mouse mouse (*screen.lanes);
    const juce::Point<int> lower (0, phazite::ui::TrackHeight::standard / 4);
    const auto onFirst = [&screen, lower] (double beat) { return screen.at (beat, 0) + lower; };
    const auto onSecond = [&screen, lower] (double beat)
    { return screen.at (beat, 1) + lower + juce::Point<int> (0, phazite::AutomationLane::defaultHeight); };

    // Four beats later along its track: the peak goes along, in the drag's one undo step.
    mouse.press (onFirst (5.0));
    mouse.dragTo (onFirst (7.0));
    mouse.dragTo (onFirst (9.0));
    mouse.release();
    mouse.pause();
    CHECK (screen.startOf (clip) == doctest::Approx (8.0));
    CHECK (valueAt (10.0) == doctest::Approx (0.9f));
    CHECK (valueAt (6.0) == doctest::Approx (0.5f));
    CHECK (phazite::describeUndo (&screen.getSong()) == juce::String ("Undo Move Clip"));
    phazite::undo (screen.getSong());
    CHECK (valueAt (6.0) == doctest::Approx (0.9f));
    phazite::test::dispatchPendingMessages();

    // With Ctrl, a copy goes, with a copy of the peak; the original keeps its own.
    mouse.press (onFirst (5.0), juce::ModifierKeys::ctrlModifier);
    mouse.dragTo (onFirst (7.0));
    mouse.dragTo (onFirst (9.0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getClipStarts (0) == std::vector { 4.0, 8.0 });
    CHECK (valueAt (6.0) == doctest::Approx (0.9f));
    CHECK (valueAt (10.0) == doctest::Approx (0.9f));
    phazite::undo (screen.getSong());
    phazite::test::dispatchPendingMessages();

    // To another track, the automation stays: that track's devices are others.
    mouse.press (onFirst (5.0));
    mouse.dragTo (onSecond (5.0) + juce::Point<int> (0, -2));
    mouse.dragTo (onSecond (5.0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getClipStarts (1) == std::vector { 4.0 });
    CHECK (valueAt (6.0) == doctest::Approx (0.9f));
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
    CHECK (phazite::getTrackIDs (screen.getSong()).size() == 4);

    // The lanes are taller than four tracks.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.doubleClick ({ 100, 4 * phazite::ui::TrackHeight::standard + 40 });
    CHECK (phazite::getTrackIDs (screen.getSong()).size() == 5);

    // Each is an undo step.
    phazite::undo (screen.getSong());
    CHECK (phazite::getTrackIDs (screen.getSong()).size() == 4);
}

TEST_CASE ("The playlist paints, with sections, a clip and a selected track")
{
    PlaylistScreen screen;
    REQUIRE (phazite::findOutline ("pop") != nullptr);
    phazite::applyOutline (screen.getSong(), *phazite::findOutline ("pop"));
    screen.addClip (1, 4.0, 8.0);
    screen.selection.selectOnly (screen.tracks[2]);
    phazite::test::dispatchPendingMessages();

    const auto image = screen.window.paintEverything();
    CHECK (image.getWidth() == 1000);
}

TEST_CASE ("A track's header shows how loud the track plays now in a narrow bar along its right edge, as tall as its row")
{
    PlaylistScreen screen;
    const auto headers = phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist);
    REQUIRE (headers.size() == 3);

    for (auto* header : headers)
    {
        // On the edge, as tall as the row above its border, apart from the switch beside it; clicks go to the header.
        auto& bar = header->getPeakBar();
        CHECK (bar.isVisible());
        CHECK (bar.getRight() == header->getWidth());
        CHECK (bar.getWidth() == phazite::ui::PeakBarTint::width);
        CHECK (bar.getY() == 0);
        CHECK (bar.getBottom() == header->getHeight() - 1);
        const auto switches = phazite::test::findAll<phazite::ui::TrackSwitch> (*header);
        REQUIRE (switches.size() == 1);
        CHECK (switches.getFirst()->getRight() < bar.getX());
        CHECK_FALSE (bar.hitTest (bar.getWidth() / 2, bar.getHeight() / 2));
    }

    // A collapsed row keeps it, as tall as the row.
    auto& first = *headers[0];
    phazite::setCollapsed (screen.context, first.getTrack(), true);
    phazite::test::dispatchPendingMessages();
    CHECK (first.getPeakBar().isVisible());
    CHECK (first.getPeakBar().getHeight() == first.getHeight() - 1);
}

TEST_CASE ("Dragging the track headers' right edge makes them wider or narrower, within limits; a double-click puts them back")
{
    using phazite::ui::Playlist;
    PlaylistScreen screen;
    auto& playlist = screen.playlist;
    auto kept = 0;
    playlist.onHeaderWidthChanged = [&kept] (int width) { kept = width; };
    const auto headers = phazite::test::findAll<phazite::ui::TrackHeader> (playlist);
    REQUIRE_FALSE (headers.isEmpty());
    CHECK (headers[0]->getWidth() == Playlist::defaultHeaderWidth);

    // In the playlist's coordinates, which stay put as the edge moves.
    const auto dragEdge = [&playlist] (int by)
    {
        auto* edge = phazite::test::findComponentWithTitle (playlist, "Track headers' edge");
        REQUIRE (edge != nullptr);
        phazite::test::Mouse mouse (playlist);
        const auto from = playlist.getLocalArea (edge, edge->getLocalBounds()).getCentre();
        mouse.press (from);
        mouse.dragTo (from + juce::Point<int> (by / 2, 0));
        mouse.dragTo (from + juce::Point<int> (by, 0));
        mouse.release();
    };

    // Wider: the headers and what is over them follow, and the app is told the width to keep as the drag ends.
    dragEdge (60);
    CHECK (playlist.getHeaderWidth() == Playlist::defaultHeaderWidth + 60);
    CHECK (headers[0]->getWidth() == playlist.getHeaderWidth());
    CHECK (kept == playlist.getHeaderWidth());

    // Never past its limits.
    dragEdge (-500);
    CHECK (playlist.getHeaderWidth() == Playlist::minHeaderWidth);
    dragEdge (800);
    CHECK (playlist.getHeaderWidth() == Playlist::maxHeaderWidth);

    // A double-click puts it back.
    auto* edge = phazite::test::findComponentWithTitle (playlist, "Track headers' edge");
    REQUIRE (edge != nullptr);
    phazite::test::Mouse mouse (*edge);
    mouse.doubleClick (edge->getLocalBounds().getCentre());
    CHECK (playlist.getHeaderWidth() == Playlist::defaultHeaderWidth);
    CHECK (kept == Playlist::defaultHeaderWidth);
}

TEST_CASE ("A track's header moves the selected tracks from anywhere on it, a double-click collapses it, and its edge resizes it")
{
    PlaylistScreen screen;
    auto headers = phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist);
    REQUIRE (headers.size() == 3);
    auto& header = *headers[0];
    REQUIRE (header.getTrack() == screen.tracks[0]);
    CHECK (phazite::test::findComponentWithTitle (header, "Collapse track") == nullptr); // no chevron, and no handle

    phazite::test::Mouse mouse (screen.playlist);
    const auto inPlaylist = [&screen, &header] (juce::Point<int> p) { return screen.playlist.getLocalPoint (&header, p); };
    const auto standard = phazite::ui::TrackHeight::standard;

    // Dragged from its middle into the lower half of the third track, it goes after that one, faded while it moves.
    const auto middle = inPlaylist (header.getLocalBounds().getCentre());
    mouse.press (middle);
    mouse.dragTo (middle + juce::Point<int> (0, 2 * standard + 10));
    CHECK (header.getAlpha() < 1.0f);
    CHECK (headers[1]->getAlpha() == 1.0f);
    mouse.release();
    mouse.pause();
    phazite::test::dispatchPendingMessages();
    CHECK (header.getAlpha() == 1.0f);
    using Tracks = std::vector<phazite::TrackID>;
    CHECK (phazite::getTrackIDs (screen.getSong()) == Tracks { screen.tracks[1], screen.tracks[2], screen.tracks[0] });
    CHECK (phazite::describeUndo (&screen.getSong()) == juce::String ("Undo Move Track"));

    // Several selected tracks move together, in their order in the playlist, by any of their headers: the third track
    // added to the selection, both dragged by the first's into the upper half of the top row, go before it.
    auto& third = *headers[2];
    mouse.click (screen.playlist.getLocalPoint (&third, third.getLocalBounds().getCentre()), juce::ModifierKeys::ctrlModifier);
    REQUIRE (phazite::getSelectedTracks (screen.selection).size() == 2);
    const auto last = inPlaylist (header.getLocalBounds().getCentre());
    mouse.press (last);
    mouse.dragTo (last - juce::Point<int> (0, 2 * standard + standard / 4));
    mouse.release();
    mouse.pause();
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getTrackIDs (screen.getSong()) == Tracks { screen.tracks[2], screen.tracks[0], screen.tracks[1] });
    CHECK (phazite::describeUndo (&screen.getSong()) == juce::String ("Undo Move Tracks"));

    // A click without a drag selects it alone; a double-click collapses it to a thin row, which shows its name.
    mouse.click (inPlaylist (header.getLocalBounds().getCentre()));
    CHECK (phazite::getSelectedTracks (screen.selection).size() == 1);
    mouse.doubleClick (inPlaylist (header.getLocalBounds().getCentre()));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::isCollapsed (screen.getSong(), screen.tracks[0]));
    CHECK (header.getHeight() == phazite::ui::TrackHeight::collapsed);
    CHECK (phazite::getTrackIDs (screen.getSong()) == Tracks { screen.tracks[2], screen.tracks[0], screen.tracks[1] }); // nothing moved
    mouse.doubleClick (inPlaylist (header.getLocalBounds().getCentre()));
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::isCollapsed (screen.getSong(), screen.tracks[0]));
    mouse.doubleClick (inPlaylist (header.getLocalBounds().getCentre()));
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::isCollapsed (screen.getSong(), screen.tracks[0]));

    // Dragging its bottom edge down expands it to where the edge is let go.
    const auto edge = inPlaylist ({ header.getWidth() / 2, header.getHeight() - 2 });
    mouse.press (edge);
    mouse.dragTo (edge + juce::Point<int> (0, 30));
    mouse.release();
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::isCollapsed (screen.getSong(), screen.tracks[0]));
    CHECK (header.getHeight() == phazite::ui::TrackHeight::collapsed + 30);
}

TEST_CASE (
    "Tracks dropped on a folder's middle go into it, at its top edge before it, at its bottom edge first in it, and never a folder into itself")
{
    PlaylistScreen screen;
    const auto folder = phazite::groupIntoFolder (screen.getSong(), { screen.tracks[2] }, nullptr); // 0, 1, the folder (2)
    REQUIRE (folder.isValid());
    phazite::test::dispatchPendingMessages();

    phazite::test::Mouse mouse (screen.playlist);
    using Tracks = std::vector<phazite::TrackID>;
    const auto order = [&screen] { return phazite::getAllPlaylistTracks (screen.context); };
    const auto headerOf = [&screen] (phazite::TrackID track) -> phazite::ui::TrackHeader&
    {
        for (auto* header : phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist))
            if (header->getTrack() == track)
                return *header;

        FAIL ("no header for the track");
        return *phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist)[0];
    };

    // Dragged by its middle (which selects it alone) to a height below the top of a track's row.
    const auto drag = [&screen, &mouse, &headerOf] (phazite::TrackID track, phazite::TrackID to, int below)
    {
        auto& header = headerOf (track);
        auto& target = headerOf (to);
        const auto from = screen.playlist.getLocalPoint (&header, header.getLocalBounds().getCentre());
        mouse.press (from);
        mouse.dragTo ({ from.x, screen.playlist.getLocalPoint (&target, juce::Point<int> (0, below)).y });
        mouse.release();
        mouse.pause();
        phazite::test::dispatchPendingMessages();
    };

    // The folder's row is 40 pixels tall: dropped in its middle, the first track goes in it, after its track.
    const auto folderHeight = phazite::ui::TrackHeight::folder;
    drag (screen.tracks[0], folder, folderHeight / 2);
    CHECK (order() == Tracks { screen.tracks[1], folder, screen.tracks[2], screen.tracks[0] });
    CHECK (phazite::isInside (screen.context, screen.tracks[0], folder));

    // Its bottom edge, while its tracks show: first in it.
    drag (screen.tracks[1], folder, folderHeight - 3);
    CHECK (order() == Tracks { folder, screen.tracks[1], screen.tracks[2], screen.tracks[0] });

    // Its top edge: before it, out of it.
    drag (screen.tracks[0], folder, 3);
    CHECK (order() == Tracks { screen.tracks[0], folder, screen.tracks[1], screen.tracks[2] });
    CHECK_FALSE (phazite::isInside (screen.context, screen.tracks[0], folder));

    // The folder among its own tracks: nothing moves. Under the last track: at the end, out of the folder.
    const auto standard = phazite::ui::TrackHeight::standard;
    drag (folder, screen.tracks[1], standard * 3 / 4);
    CHECK (order() == Tracks { screen.tracks[0], folder, screen.tracks[1], screen.tracks[2] });
    drag (screen.tracks[1], screen.tracks[2], standard * 2);
    CHECK (order() == Tracks { screen.tracks[0], folder, screen.tracks[2], screen.tracks[1] });
    CHECK_FALSE (phazite::isInside (screen.context, screen.tracks[1], folder));
    CHECK (phazite::getTrackRow (screen.context, screen.tracks[2])->folderColours.size() == 1);
}

TEST_CASE ("The button over the track headers, and Alt+U, collapse every track and lane, and expand them all again")
{
    PlaylistScreen screen;
    phazite::showAutomationLane (screen.context, phazite::getVolume (screen.context, screen.tracks[0]).getRef());
    phazite::test::dispatchPendingMessages();
    auto* button = phazite::test::findComponentWithTitle (screen.playlist, "Collapse all");
    REQUIRE (button != nullptr);
    CHECK (button->isShowing());

    phazite::test::Mouse mouse (*button);
    mouse.click (button->getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();

    for (const auto track : screen.tracks)
        CHECK (phazite::isCollapsed (screen.getSong(), track));

    CHECK (phazite::getAutomationLanes (screen.context, screen.tracks[0])[0].collapsed);
    CHECK (phazite::test::findComponentWithTitle (screen.playlist, "Expand all") == button);
    CHECK (phazite::getCurrentName (*screen.registry.find ("track.collapseAll")) == juce::String ("Expand All"));
    CHECK_FALSE (screen.getSong().getUndoManager().canUndo()); // view state

    // Alt+U, as in Ableton Live.
    screen.window.pressKey (juce::KeyPress ('u', juce::ModifierKeys::altModifier, 0));
    screen.window.releaseKeys();
    phazite::test::dispatchPendingMessages();

    for (const auto track : screen.tracks)
        CHECK_FALSE (phazite::isCollapsed (screen.getSong(), track));

    CHECK_FALSE (phazite::getAutomationLanes (screen.context, screen.tracks[0])[0].collapsed);
    CHECK (phazite::test::findComponentWithTitle (screen.playlist, "Collapse all") == button);
}

TEST_CASE ("With FL Studio's keymap, a click on empty space places the last clip used, and right-click deletes it")
{
    PlaylistScreen screen;
    screen.commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("FL Studio"));
    const auto clip = screen.addClip (0, 0.0, 4.0);
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
    const auto clip = screen.addClip (0, 0.0, 4.0);
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

    // Ctrl+drag over empty space selects a time range (a plain click would place a clip), and it loops.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.press (screen.at (20.0, 1), juce::ModifierKeys::commandModifier);
    mouse.dragTo (screen.at (28.0, 1));
    mouse.release();
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (screen.range.isEmpty());
    CHECK (phazite::isLoopOn (screen.getSong()));
    CHECK_FALSE (phazite::doesLoopFollowSong (screen.getSong()));
    const auto loop = phazite::getLoopBeats (screen.getSong());
    CHECK (loop.getStart().inBeats() == doctest::Approx (screen.range.getRange().getStart().inBeats()));
    CHECK (loop.getEnd().inBeats() == doctest::Approx (screen.range.getRange().getEnd().inBeats()));

    // Without a selection, the song loops again.
    mouse.click (screen.at (2.0, 2), juce::ModifierKeys::commandModifier);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.range.isEmpty());
    CHECK (phazite::doesLoopFollowSong (screen.getSong()));
    CHECK (phazite::getLoopBeats (screen.getSong()).getEnd().inBeats() == doctest::Approx (16.0));
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
    REQUIRE (phazite::isLoopOn (screen.getSong()));

    screen.window.pressKey (juce::KeyPress::createFromDescription ("command + B"));
    phazite::test::dispatchPendingMessages();

    // What was in the range is there again right after it, the range is the copy, and the loop went with it.
    const auto starts = screen.getClipStarts (0);
    REQUIRE (starts.size() == 2);
    CHECK (starts[1] == doctest::Approx (8.0));
    CHECK (screen.getClipStarts (1).empty());
    CHECK (screen.getSong().getUndoManager().getUndoDescription() == juce::String ("Duplicate Range"));
    CHECK (screen.range.getRange().getStart().inBeats() == doctest::Approx (8.0));
    CHECK (screen.range.getRange().getEnd().inBeats() == doctest::Approx (16.0));
    const auto loop = phazite::getLoopBeats (screen.getSong());
    CHECK (loop.getStart().inBeats() == doctest::Approx (8.0));
    CHECK (loop.getEnd().inBeats() == doctest::Approx (16.0));

    // Undo takes the copy back and puts the range back, and the loop follows the range there.
    phazite::undo (screen.getSong());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getClipStarts (0).size() == 1);
    CHECK (screen.range.getRange().getStart().inBeats() == doctest::Approx (0.0));
    CHECK (screen.range.getRange().getEnd().inBeats() == doctest::Approx (8.0));
    const auto loopAfterUndo = phazite::getLoopBeats (screen.getSong());
    CHECK (loopAfterUndo.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (loopAfterUndo.getEnd().inBeats() == doctest::Approx (8.0));
}

TEST_CASE (
    "The playlist's vertical scroll bar shows only while its tracks don't fit, thin, over the lanes' edge, and the time line has no other")
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

    // The overview of the song along the time bar's top scrolls the time line, as FL Studio's: no scroll bar under the lanes.
    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
        if (! bar->isVertical())
            CHECK_FALSE (bar->isVisible());

    // Tracks that don't fit: the bar shows over the lanes' right edge, thin, and the lanes keep their whole width; it is drawn in
    // front of them, over a see-through track.
    phazite::ensureTracks (screen.getSong(), 30);
    phazite::test::dispatchPendingMessages();
    CHECK (vertical->isVisible());
    CHECK (screen.lanes->getRight() == screen.playlist.getWidth());
    CHECK (vertical->getWidth() == phazite::ui::ScrollTint::overlayWidth);
    CHECK (vertical->getRight() == screen.lanes->getRight());
    CHECK (vertical->getY() == screen.lanes->getY());
    CHECK (screen.playlist.getIndexOfChildComponent (vertical) > screen.playlist.getIndexOfChildComponent (screen.lanes));
    CHECK_FALSE (vertical->findColour (juce::ScrollBar::trackColourId).isTransparent());
    CHECK (vertical->findColour (juce::ScrollBar::thumbColourId).getAlpha() < 255);

    std::vector<phazite::TrackID> added;

    for (const auto track : phazite::getTrackIDs (screen.getSong()))
        if (std::find (screen.tracks.begin(), screen.tracks.end(), track) == screen.tracks.end())
            added.push_back (track);

    phazite::deleteTracks (screen.getSong(), added, nullptr);

    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (vertical->isVisible());
    CHECK (screen.lanes->getRight() == screen.playlist.getWidth());
}

TEST_CASE (
    "The overview along the time bar's top shows the whole song; its thumb drags to scroll, its end to zoom, a press elsewhere goes there")
{
    PlaylistScreen screen;
    REQUIRE (screen.addClip (0, 0.0, 4.0).isValid());
    REQUIRE (screen.addClip (2, 32.0, 4.0).isValid());
    phazite::test::dispatchPendingMessages();

    auto* overview = phazite::test::findComponentWithTitle (screen.playlist, "Overview");
    REQUIRE (overview != nullptr);
    CHECK (overview->getY() < screen.lanes->getY());
    CHECK (overview->getX() == screen.lanes->getX());
    CHECK (overview->getRight() == screen.lanes->getRight());

    // The time line's range and the part in view, as its hidden scroll bar keeps them: where the overview puts its thumb.
    juce::ScrollBar* timeline = nullptr;

    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
        if (! bar->isVertical())
            timeline = bar;

    REQUIRE (timeline != nullptr);
    const auto xFor = [&] (double beat)
    {
        const auto whole = timeline->getRangeLimit();
        return (float) ((beat - whole.getStart()) / whole.getLength() * overview->getWidth());
    };

    // Dragging the thumb scrolls the playlist along.
    phazite::test::Mouse mouse (*overview);
    const auto startBefore = screen.playlist.getView().start.inBeats();
    const auto view = timeline->getCurrentRange();
    const auto middle = juce::Point<int> (juce::roundToInt (xFor (view.getStart() + view.getLength() / 2.0)), overview->getHeight() / 2);
    mouse.press (middle);
    mouse.dragTo (middle + juce::Point<int> (20, 0));
    mouse.release();
    CHECK (screen.playlist.getView().start.inBeats() > startBefore);

    // Dragging its right end zooms out, its left end staying where it was.
    const auto zoomBefore = screen.playlist.getView().pixelsPerBeat;
    const auto startNow = screen.playlist.getView().start.inBeats();
    const auto end = juce::Point<int> (juce::roundToInt (xFor (timeline->getCurrentRange().getEnd())), overview->getHeight() / 2);
    mouse.press (end);
    mouse.dragTo (end + juce::Point<int> (30, 0));
    mouse.release();
    CHECK (screen.playlist.getView().pixelsPerBeat < zoomBefore);
    CHECK (screen.playlist.getView().start.inBeats() == doctest::Approx (startNow));

    // A press away from the thumb brings that part of the song into view, centred.
    const auto far = juce::Point<int> (overview->getWidth() - 4, overview->getHeight() / 2);
    const auto beatThere =
        timeline->getRangeLimit().getStart() + (double) far.x / overview->getWidth() * timeline->getRangeLimit().getLength();
    mouse.click (far);
    const auto shown = timeline->getCurrentRange();
    CHECK (shown.contains (beatThere));
}

TEST_CASE ("Every clip shows in its track's color, and the rows themselves aren't colored")
{
    PlaylistScreen screen;
    REQUIRE (screen.addClip (0, 0.0, 4.0).isValid());
    phazite::test::dispatchPendingMessages();

    const auto clipComponents = phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist);
    REQUIRE (clipComponents.size() == 1);
    CHECK (clipComponents[0]->getColour() == phazite::getTrackColour (screen.getSong(), screen.tracks[0], 0));

    // An empty stretch of an unselected track's row is the playlist's background, between grid lines.
    const auto image = screen.window.paintEverything();
    const auto point = screen.lanes->getBounds().getPosition() + screen.at (10.37, 1);
    CHECK (image.getPixelAt (point.x, point.y) == phazite::ui::getPalette (screen.playlist).background);
}

TEST_CASE ("A folder's row shows its tracks' clips in their colors, collapsed too, with no components of its own")
{
    PlaylistScreen screen;
    screen.addClip (1, 4.0, 4.0);
    const auto folder = phazite::groupIntoFolder (screen.getSong(), { screen.tracks[1] }, nullptr);
    REQUIRE (folder.isValid());
    phazite::setCollapsed (screen.context, folder, true);
    phazite::test::dispatchPendingMessages();

    // Only the folder's header shows of it: its track is hidden, and its clip has no component.
    const auto headers = phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist);
    auto* header = std::find_if (headers.begin(), headers.end(),
                                 [folder] (phazite::ui::TrackHeader* each) { return each->getTrack() == folder && each->isShowing(); });
    REQUIRE (header != headers.end());
    CHECK (phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist).isEmpty());
    CHECK ((*header)->getHeight() == phazite::ui::TrackHeight::folder); // shorter than a track's at first: it holds no clips

    // The row shows the clip, in its track's color, in the middle of its row (between grid lines); and nothing after it.
    const auto row = screen.lanes->getLocalArea (*header, (*header)->getLocalBounds());
    const auto image = screen.window.paintEverything();
    const auto lanesAt = screen.lanes->getBounds().getPosition();
    const auto xOf = [&screen, lanesAt] (double beat)
    { return lanesAt.x + juce::roundToInt (screen.playlist.getView().beatToX (beats (beat))); };
    const auto y = lanesAt.y + row.getCentreY();
    const auto trackColour = phazite::getTrackRow (screen.context, screen.tracks[1])->colour;
    const auto background = phazite::ui::getPalette (screen.playlist).background;
    const auto expected = background.overlaidWith (trackColour.withAlpha (phazite::ui::FolderTint::clip));
    const auto shown = image.getPixelAt (xOf (5.37), y);
    CHECK (std::abs ((int) shown.getRed() - (int) expected.getRed()) <= 2);
    CHECK (std::abs ((int) shown.getGreen() - (int) expected.getGreen()) <= 2);
    CHECK (std::abs ((int) shown.getBlue() - (int) expected.getBlue()) <= 2);
    CHECK (image.getPixelAt (xOf (10.37), y) == background);
}

TEST_CASE ("Only the clips in view have components, which come and go as the playlist scrolls and zooms")
{
    // A song much bigger than the playlist (docs/performance-audit.md, section 8): its clips once all had components, each
    // moved at every step of a scroll, and all built again at each change of a clip.
    PlaylistScreen screen;
    screen.tracks = phazite::ensureTracks (screen.getSong(), 40);

    for (int track = 0; track < 40; ++track)
        for (int c = 0; c < 30; ++c)
            screen.addClip (track, c * 8.0 + 4.0, 4.0);

    // Two that overlap on the first track, one under the other: the second, made later, below.
    const auto first = screen.addClip (0, 0.0, 2.0);
    const auto second = screen.addClip (0, 1.0, 2.0);
    phazite::test::dispatchPendingMessages();

    const auto componentOf = [&screen] (phazite::ClipID clip) -> phazite::ui::ClipComponent*
    {
        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (component->getClip() == clip)
                return component;

        return nullptr;
    };

    // Of 1202 clips, those that show: a few bars of the tracks in view, all bars of them once zoomed out.
    const auto checkOnlyInView = [&screen] (int most)
    {
        const auto components = phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist);
        CHECK_FALSE (components.isEmpty());
        CHECK (components.size() <= most);

        for (auto* component : components)
            CHECK (component->getParentComponent()->getLocalBounds().intersects (component->getBounds()));
    };

    checkOnlyInView (100);
    const auto lastTrackClips = phazite::getClips (screen.getSong(), screen.tracks[39]);
    REQUIRE_FALSE (lastTrackClips.empty());
    CHECK (componentOf (lastTrackClips.front()) == nullptr);
    REQUIRE (componentOf (first) != nullptr);
    REQUIRE (componentOf (second) != nullptr);
    CHECK (screen.lanes->getIndexOfChildComponent (componentOf (first)->getParentComponent()) >= 0);

    // Scrolled down to the last track: its clips have components, the first track's none, and a click finds what is there.
    phazite::test::Mouse mouse (*screen.lanes);
    const auto middle = screen.lanes->getLocalBounds().getCentre();

    for (int notch = 0; notch < 60; ++notch)
        mouse.wheel (middle, -60.0f / 256.0f);

    checkOnlyInView (100);
    CHECK (componentOf (first) == nullptr);
    auto* last = componentOf (lastTrackClips.front());
    REQUIRE (last != nullptr);
    mouse.click (screen.lanes->getLocalPoint (last, last->getLocalBounds().getCentre()));
    CHECK (screen.selection.isSelected (lastTrackClips.front()));

    // Back at the top, scrolled along so that only the second of the two that overlap shows, and back: the first comes
    // into view before the second in their layer, as the playlist lists them (where clips crossfade, the later shows over
    // the earlier), and stays there. A click in the lower half of the row finds the second.
    for (int notch = 0; notch < 60; ++notch)
        mouse.wheel (middle, 60.0f / 256.0f);

    screen.playlist.setViewStart (phazite::BeatPosition::fromBeats (2.5));
    CHECK (componentOf (first) == nullptr);
    CHECK (componentOf (second) != nullptr);
    screen.playlist.setViewStart ({});
    auto* firstShown = componentOf (first);
    auto* secondShown = componentOf (second);
    REQUIRE (firstShown != nullptr);
    REQUIRE (secondShown != nullptr);
    REQUIRE (firstShown->getParentComponent() == secondShown->getParentComponent());
    const auto* layer = firstShown->getParentComponent();
    CHECK (layer->getIndexOfChildComponent (firstShown) < layer->getIndexOfChildComponent (secondShown));
    mouse.click (screen.at (1.5, 0) + juce::Point<int> (0, phazite::ui::TrackHeight::standard / 4));
    CHECK (screen.selection.isSelected (second));

    // Zoomed out, every clip of the tracks in view shows, the song's last ones too.
    screen.playlist.zoomToFit();
    checkOnlyInView (400);

    for (const auto clip : phazite::getClips (screen.getSong(), screen.tracks[0]))
        CHECK (componentOf (clip) != nullptr);
}

TEST_CASE ("Clips that overlap on a track show one under another, and each is grabbed where it shows")
{
    // A two-bar clip and another a bar later on the first track, which play together: they used to show one over the other,
    // where only the later one could be grabbed.
    PlaylistScreen screen;
    const auto first = screen.addClip (0, 0.0, 8.0);
    const auto second = screen.addClip (0, 4.0, 8.0);
    const auto alone = screen.addClip (1, 0.0, 8.0);
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();

    // The first, made first, above the second, a pixel apart, together as tall as a clip alone on its track.
    const auto top = screen.boundsOf (first), bottom = screen.boundsOf (second), whole = screen.boundsOf (alone);
    REQUIRE_FALSE (top.isEmpty());
    REQUIRE_FALSE (bottom.isEmpty());
    CHECK (top.getY() == 1);
    CHECK (bottom.getY() == top.getBottom() + 1);
    CHECK (bottom.getBottom() == phazite::ui::TrackHeight::standard - 2);
    CHECK (whole.getHeight() == phazite::ui::TrackHeight::standard - 3);

    // Where they overlap, each is clicked in its own half.
    phazite::test::Mouse mouse (*screen.lanes);
    const auto x = screen.at (6.0, 0).x;
    mouse.click ({ x, top.getCentreY() });
    mouse.pause();
    CHECK (screen.selection.isSelected (first));
    CHECK_FALSE (screen.selection.isSelected (second));
    mouse.click ({ x, bottom.getCentreY() });
    mouse.pause();
    CHECK (screen.selection.isSelected (second));
    CHECK_FALSE (screen.selection.isSelected (first));

    // The first, dragged from there, goes, and the second stays; still overlapping, each keeps its place.
    mouse.press ({ x, top.getCentreY() });
    mouse.dragTo ({ screen.at (8.0, 0).x, top.getCentreY() });
    mouse.dragTo ({ screen.at (10.0, 0).x, top.getCentreY() });
    mouse.release();
    mouse.pause();
    phazite::test::dispatchPendingMessages();
    CHECK (screen.startOf (first) == doctest::Approx (4.0));
    CHECK (screen.startOf (second) == doctest::Approx (4.0));
    CHECK (screen.boundsOf (first).getY() == top.getY());
    CHECK (screen.boundsOf (second).getY() == bottom.getY());

    // Once they no longer overlap, each has the whole row again.
    phazite::moveClip (screen.context, second, phazite::BeatPosition::fromBeats (12.0), screen.tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.boundsOf (first).getHeight() == whole.getHeight());
    CHECK (screen.boundsOf (second).getHeight() == whole.getHeight());
}

TEST_CASE ("Dragging along the top of the ruler loops part of the song, and the loop's edges and middle change it")
{
    PlaylistScreen screen;
    screen.addClip (0, 0.0, 16.0); // four bars
    phazite::test::dispatchPendingMessages();

    auto* ruler = dynamic_cast<phazite::ui::TimelineRuler*> (phazite::test::findComponentWithTitle (screen.playlist, "Timeline"));
    REQUIRE (ruler != nullptr);
    const auto lane = ruler->getLoopLane().getCentreY(), lower = ruler->getHeight() - 4;
    const auto x = [&screen] (double beat) { return juce::roundToInt (screen.playlist.getView().beatToX (beats (beat))); };
    const auto isLoop = [&screen] (double start, double end)
    {
        const auto loop = phazite::getLoopBeatRange (screen.getSong());
        return loop.has_value() && std::abs (loop->first - start) < 1.0e-6 && std::abs (loop->second - end) < 1.0e-6;
    };

    phazite::test::Mouse mouse (*ruler);

    // A drag along the top loops what it covers, on the grid.
    mouse.press ({ x (4.1), lane });
    mouse.dragTo ({ x (8.1), lane });
    mouse.release();
    CHECK (isLoop (4.0, 8.0));
    CHECK_FALSE (phazite::doesLoopFollowSong (screen.getSong()));

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
    CHECK_FALSE (phazite::getLoopBeatRange (screen.getSong()).has_value());
    mouse.click ({ x (20.0), lane });
    CHECK (isLoop (6.0, 14.0));
    mouse.doubleClick ({ x (20.0), lane });
    CHECK (phazite::isLoopingSong (screen.getSong()));

    // Lower down, a click moves the playhead and leaves the loop alone; Ctrl+drag there makes a loop, as in FL Studio.
    mouse.click ({ x (3.0), lower });
    CHECK (screen.playlist.getPlayheadBeat().inBeats() == doctest::Approx (3.0));
    CHECK (phazite::isLoopingSong (screen.getSong()));
    mouse.press ({ x (1.0), lower }, juce::ModifierKeys::commandModifier);
    mouse.dragTo ({ x (2.0), lower });
    mouse.release();
    CHECK (isLoop (1.0, 2.0));
}

TEST_CASE ("While the song plays, the playlist turns the page at the playhead, until it is scrolled away until it plays again")
{
    PlaylistScreen screen;
    screen.addClip (0, 0.0, 256.0);
    screen.playlist.zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (16.0) });
    const auto viewStart = [&screen] { return screen.playlist.getView().start.inBeats(); };
    const auto playTo = [&screen] (double beat) { phazite::movePlayhead (screen.getSong(), beats (beat)); };

    phazite::playOrStop (screen.getSong());
    REQUIRE (phazite::isSongPlaying (screen.getSong()));

    // Past the right edge, the page turns.
    playTo (20.0);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return viewStart() > 16.0; }));

    // Scrolled back while it plays (Shift and the wheel), the view stays there as the playhead plays on out of it.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.wheel ({ 10, 10 }, 1.0f, juce::ModifierKeys::shiftModifier);
    const auto scrolledTo = viewStart();
    REQUIRE (scrolledTo < 19.0);
    CHECK (screen.playlist.isFollowingPaused());
    playTo (40.0);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 300);
    CHECK (viewStart() == doctest::Approx (scrolledTo));

    // Stopped and played again, from where it was moved to last, the view follows it there.
    phazite::playOrStop (screen.getSong());
    phazite::test::dispatchMessagesUntil ([] { return false; }, 300);
    phazite::playOrStop (screen.getSong());
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return viewStart() > 30.0; }));
    CHECK_FALSE (screen.playlist.isFollowingPaused());
    phazite::playOrStop (screen.getSong());
}

TEST_CASE ("Scrolling continuously, the playlist keeps the playhead in the middle while the song plays, once it gets there")
{
    PlaylistScreen screen;
    screen.addClip (0, 0.0, 256.0);
    screen.playlist.zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (16.0) });
    screen.playlist.setScrollingContinuously (true);
    const auto viewStart = [&screen] { return screen.playlist.getView().start.inBeats(); };
    const auto playTo = [&screen] (double beat) { phazite::movePlayhead (screen.getSong(), beats (beat)); };

    phazite::playOrStop (screen.getSong());
    REQUIRE (phazite::isSongPlaying (screen.getSong()));

    // Before the middle of the view (8 of its 16 beats), the playhead crosses it; after, the song scrolls under it, a pixel
    // at a time.
    playTo (3.0);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 300);
    CHECK (viewStart() == doctest::Approx (0.0));
    playTo (40.0);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return std::abs (viewStart() - 32.0) < 0.05; }));
    playTo (40.5);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return std::abs (viewStart() - 32.5) < 0.05; }));

    // Scrolled away while it plays, it stays there.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.wheel ({ 10, 10 }, 1.0f, juce::ModifierKeys::shiftModifier);
    const auto scrolledTo = viewStart();
    playTo (41.0);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 300);
    CHECK (viewStart() == doctest::Approx (scrolledTo));
    phazite::playOrStop (screen.getSong());
}

TEST_CASE ("The playhead drags along the ruler to the grid; a song playing is silent until it is let go, then plays on from there")
{
    PlaylistScreen screen;
    screen.addClip (0, 0.0, 16.0);
    phazite::test::dispatchPendingMessages();

    auto* ruler = dynamic_cast<phazite::ui::TimelineRuler*> (phazite::test::findComponentWithTitle (screen.playlist, "Timeline"));
    REQUIRE (ruler != nullptr);
    const auto y = ruler->getHeight() - 4;
    const auto x = [&screen] (double beat) { return juce::roundToInt (screen.playlist.getView().beatToX (beats (beat))); };
    const auto playhead = [&screen] { return screen.playlist.getPlayheadBeat().inBeats(); };
    auto& song = screen.getSong();

    // Stopped: a press takes it there, and a drag along, line by line of the grid; playback starts where it is let go.
    phazite::test::Mouse mouse (*ruler);
    mouse.press ({ x (3.0), y });
    CHECK (playhead() == doctest::Approx (3.0));
    mouse.dragTo ({ x (4.0) + 1, y });
    CHECK (playhead() == doctest::Approx (4.0));
    mouse.dragTo ({ x (6.0), y });
    CHECK (playhead() == doctest::Approx (6.0));
    mouse.release();
    CHECK_FALSE (phazite::isSongPlaying (song));
    CHECK (phazite::getPlayStartBeat (song).inBeats() == doctest::Approx (6.0));

    // Playing: the press pauses the song, the drag takes the playhead along in silence, and let go, the song plays on from
    // there.
    phazite::playOrStop (song);
    REQUIRE (phazite::isSongPlaying (song));
    mouse.press ({ x (2.0), y });
    CHECK_FALSE (phazite::isSongPlaying (song));
    CHECK (playhead() == doctest::Approx (2.0));
    mouse.dragTo ({ x (8.0), y });
    CHECK_FALSE (phazite::isSongPlaying (song));
    CHECK (playhead() == doctest::Approx (8.0));
    mouse.release();
    CHECK (phazite::isSongPlaying (song));
    CHECK (playhead() >= 8.0 - 1.0e-6);
    CHECK (playhead() < 9.0);
    phazite::playOrStop (song);
    CHECK (playhead() == doctest::Approx (8.0)); // stopped, back where it was let go
}

TEST_CASE ("The playhead is a marker at the foot of the ruler with a line from its point down, in the focus colour")
{
    PlaylistScreen screen;
    phazite::test::dispatchPendingMessages();
    auto* ruler = phazite::test::findComponentWithTitle (screen.playlist, "Timeline");
    auto* playhead = phazite::test::findComponentWithTitle (screen.playlist, "Playhead");
    REQUIRE (ruler != nullptr);
    REQUIRE (playhead != nullptr);
    REQUIRE (playhead->isVisible());

    // Its top is the marker's, at the foot of the ruler: nothing of it above, in the bar numbers.
    CHECK (playhead->getY() == ruler->getBottom() - phazite::ui::PlayheadTint::markerHeight);
    CHECK (playhead->getBottom() > ruler->getBottom());
    CHECK (playhead->getX() + playhead->getWidth() / 2 == ruler->getX() + juce::roundToInt (screen.playlist.getView().beatToX ({})));

    // Painted: the marker filled across its top, the line under its point, in the focus colour; clear beside the line.
    phazite::ui::LookAndFeel lookAndFeel;
    playhead->setLookAndFeel (&lookAndFeel);
    const auto focus = phazite::ui::getPalette (*playhead).focus;
    juce::Image image (juce::Image::ARGB, playhead->getWidth(), playhead->getHeight(), true, juce::SoftwareImageType());
    {
        juce::Graphics g (image);
        playhead->paintEntireComponent (g, false);
    }
    playhead->setLookAndFeel (nullptr);

    const auto isFocus = [&focus] (juce::Colour c) { return c.getAlpha() > 200 && std::abs (c.getRed() - focus.getRed()) < 8; };
    const auto centre = playhead->getWidth() / 2;
    CHECK (isFocus (image.getPixelAt (centre, 2)));
    CHECK (isFocus (image.getPixelAt (1, 3)));
    CHECK (isFocus (image.getPixelAt (centre, playhead->getHeight() - 2)));
    CHECK (image.getPixelAt (1, playhead->getHeight() - 2).getAlpha() == 0);
}

TEST_CASE ("The sections strip adds, selects, resizes, moves and copies sections with the mouse")
{
    PlaylistScreen screen;
    auto& context = screen.context;
    auto& undo = screen.getSong().getUndoManager();
    screen.playlist.zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (64.0) });

    auto* strip = phazite::test::findComponentWithTitle (screen.playlist, "Sections");
    REQUIRE (strip != nullptr);
    const auto y = strip->getHeight() / 2;
    const auto x = [&screen] (double beat)
    { return juce::roundToInt (screen.playlist.getView().beatToX (phazite::BeatPosition::fromBeats (beat))); };
    const auto sectionNamed = [&context] (const char* name)
    {
        for (const auto& section : phazite::getSongSections (context))
            if (section.name == juce::String (name))
                return std::optional (section);

        return std::optional<phazite::SongSection>();
    };

    phazite::test::Mouse mouse (*strip);

    // A double-click adds a section from the start of the bar, eight bars long, selected alone.
    mouse.doubleClick ({ x (2.0), y });
    mouse.doubleClick ({ x (34.0), y });
    REQUIRE (phazite::getSongSections (context).size() == 2);
    const auto first = sectionNamed ("Section 1"), second = sectionNamed ("Section 2");
    REQUIRE (first.has_value());
    REQUIRE (second.has_value());
    CHECK (second->range.getStart().inBeats() == doctest::Approx (32.0));
    CHECK (context.getSongSelection().getSections() == std::vector { second->id });

    // A click selects one alone.
    mouse.click ({ x (8.0), y });
    CHECK (context.getSongSelection().getSections() == std::vector { first->id });

    // Dragging its right edge makes it shorter, and what comes after moves along.
    mouse.press ({ x (32.0) - 2, y });
    mouse.dragTo ({ x (16.0), y });
    mouse.release();
    CHECK (undo.getUndoDescription() == juce::String ("Resize Section"));
    CHECK (sectionNamed ("Section 1")->range.getEnd().inBeats() == doctest::Approx (16.0));
    CHECK (sectionNamed ("Section 2")->range.getStart().inBeats() == doctest::Approx (16.0));

    // Dragging it moves it to the boundary nearest the mouse: the end of the song.
    mouse.pause();
    mouse.press ({ x (8.0), y });
    mouse.dragTo ({ x (46.0), y });
    mouse.release();
    CHECK (undo.getUndoDescription() == juce::String ("Move Section"));
    CHECK (sectionNamed ("Section 2")->range.getStart().inBeats() == doctest::Approx (0.0));
    CHECK (sectionNamed ("Section 1")->range.getStart().inBeats() == doctest::Approx (32.0));

    // Ctrl+drag copies it there, and selects the copy.
    mouse.pause();
    mouse.press ({ x (4.0), y }, juce::ModifierKeys::commandModifier);
    mouse.dragTo ({ x (47.0), y });
    mouse.release();
    CHECK (undo.getUndoDescription() == juce::String ("Copy Section"));
    const auto sections = phazite::getSongSections (context);
    REQUIRE (sections.size() == 3);
    CHECK (sections[2].name == juce::String ("Section 2"));
    CHECK (sections[2].range.getStart().inBeats() == doctest::Approx (48.0));
    CHECK (context.getSongSelection().getSections() == std::vector { sections[2].id });
}

TEST_CASE ("A sample that follows the song's tempo is marked in the playlist, so a change of tempo stretches no surprise")
{
    PlaylistScreen screen;
    phazite::test::TemporaryFolder temp;
    REQUIRE (phazite::importFile (screen.getSong(), screen.tracks[0],
                                  phazite::test::writeSound (temp.folder.getChildFile ("Beat 120 BPM.wav"), 2.0), beats (0.0))
                 .isValid());
    REQUIRE (phazite::importFile (screen.getSong(), screen.tracks[1],
                                  phazite::test::writeSound (temp.folder.getChildFile ("Hit sound.wav"), 0.5), beats (0.0))
                 .isValid());
    phazite::test::dispatchPendingMessages();

    // The loop, which knows its tempo, follows the song's; the hit plays at its own speed.
    const auto findClip = [&screen] (const juce::String& name)
    {
        auto* clip = dynamic_cast<phazite::ui::ClipComponent*> (phazite::test::findComponentWithTitle (screen.playlist, name));
        REQUIRE (clip != nullptr);
        return clip;
    };

    CHECK (findClip ("Beat 120 BPM")->getDescription() == juce::String ("Stretches with the song's tempo"));
    CHECK (findClip ("Hit sound")->getDescription().isEmpty());

    // Samples Follow Tempo: every one of them does.
    {
        const phazite::UndoStep step (screen.getSong(), "Samples Follow Tempo");
        phazite::toggleSamplesFollowTempo (screen.getSong());
    }

    phazite::test::dispatchPendingMessages();
    CHECK (findClip ("Hit sound")->getDescription() == juce::String ("Stretches with the song's tempo"));
}

TEST_CASE ("While an audio clip's render is being made it shows faint, with how far the render has come along its bottom edge")
{
    PlaylistScreen screen;
    phazite::test::TemporaryFolder temp;
    const auto id = phazite::importFile (screen.getSong(), screen.tracks[0],
                                         phazite::test::writeClicks (temp.folder.getChildFile ("Long loop"), 64, 120.0), beats (0.0));
    REQUIRE (phazite::isAudioState (phazite::findClipState (screen.getSong(), id)));
    phazite::test::dispatchPendingMessages();
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;

    // What its component shows: how far its render has come, or nothing; and nothing at all without a component.
    const auto shownRendering = [&screen, &ticker, id]() -> std::optional<std::optional<double>>
    {
        ticker->tickNow();

        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (component->getClip() == id)
                return component->getRendering();

        return std::nullopt;
    };

    REQUIRE (shownRendering().has_value());
    CHECK_FALSE (shownRendering()->has_value()); // at its own tempo it plays its file

    // Slower, it waits for its render, which a while later it plays.
    phazite::setTempo (screen.getSong(), 100.0);
    REQUIRE (
        phazite::test::dispatchMessagesUntil ([&shownRendering] { return shownRendering().value_or (std::nullopt).has_value(); }, 10000));
    const auto progress = *shownRendering().value_or (std::nullopt);
    CHECK (progress >= 0.0);
    CHECK (progress < 1.0);
    CHECK (phazite::test::dispatchMessagesUntil ([&shownRendering] { return ! shownRendering().value_or (0.0).has_value(); }, 120000));
}

TEST_CASE ("A track without an instrument has an arm switch: with an input, a click arms it, and its header says what it records")
{
    PlaylistScreen screen;
    const auto findArm = [&screen] (int track) -> juce::Button*
    {
        for (auto* header : phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist))
            if (header->getTrack() == screen.tracks[(size_t) track])
                return dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*header, "Arm"));

        return nullptr;
    };

    // A track with an instrument plays what is played on MIDI: no arm switch.
    auto* arm = findArm (0);
    REQUIRE (arm != nullptr);
    CHECK (arm->isVisible());
    phazite::setInstrument (screen.getSong(), screen.tracks[1], phazite::makeOwnDeviceState (screen.getSong(), "phaziteSynth"));
    phazite::test::dispatchPendingMessages();
    REQUIRE (findArm (1) != nullptr);
    CHECK_FALSE (findArm (1)->isVisible());

    // With an input, a click arms the track; its header says what it records, and the click is no undo step.
    phazite::setTrackInput (screen.context, screen.tracks[0], "1");
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::Mouse mouse (*arm);
    mouse.click (arm->getLocalBounds().getCentre());
    CHECK (phazite::isArmed (screen.context, screen.tracks[0]));
    CHECK (arm->getToggleState());
    CHECK_FALSE (screen.getSong().getUndoManager().canUndo());
    const auto labels = phazite::test::findAll<juce::Label> (screen.playlist);
    CHECK (
        std::any_of (labels.begin(), labels.end(), [] (juce::Label* label) { return label->isVisible() && label->getText() == "In 1"; }));

    // Armed elsewhere (the Arm Track command, the mixer), it shows at the displays' next tick.
    phazite::setArmed (screen.context, screen.tracks[0], false);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK_FALSE (arm->getToggleState());
}
