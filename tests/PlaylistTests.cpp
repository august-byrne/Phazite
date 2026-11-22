// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist, driven with the mouse and the keys as the user would.
#include "AppScreen.h"
#include "EnginePlayback.h"
#include "TestClips.h"
#include "UiTestHelpers.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
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
#include "model/notes/NotesModel.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/components/Controls.h"
#include "ui/components/DisplayTicker.h"
#include "ui/components/TrackSwitch.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/playlist/TrackHeader.h"
#include "ui/playlist/TrackHeights.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>

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
        REQUIRE (dynamic_cast<phazite::ui::TimelinePartWindow*> (lanes->getParentComponent()) != nullptr);
    }

    // Where the lanes show, in the playlist: the window they show through, which they are a pixel wider than
    // (TimelineEditor::addTimelinePart()). At rest, their own x 0 is its left.
    juce::Rectangle<int> lanesArea() const { return lanes->getParentComponent()->getBounds(); }

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
    phazite::ui::KeyDispatcher shortcutKeys { commands, phazite::ui::getUsLayout() };
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
        mouse.dragTo ({ screen.lanesArea().getWidth() + 40 + i % 2, start.y });

    mouse.release();

    const auto& view = screen.playlist.getView();
    CHECK (view.start.inBeats() > 0.0);

    // The clip went along, to beyond what showed when the drag began.
    const auto widthInBeats = screen.lanesArea().getWidth() / view.pixelsPerBeat;
    CHECK (screen.startOf (clip) > widthInBeats);
}

TEST_CASE ("While a clip is dragged, the playlist says where it starts, and trimmed, where its edge is and how long it is")
{
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 4.0, 4.0);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.playlist.getDragReadout().isEmpty());

    // Moved by its lower half, four beats on (bar 3 in 4/4).
    phazite::test::Mouse mouse (*screen.lanes);
    const auto lower = phazite::ui::TrackHeight::standard / 4;
    const auto from = screen.at (6.0, 0).translated (0, lower);
    mouse.press (from);
    mouse.dragTo (from.translated (3, 0));
    mouse.dragTo (screen.at (10.0, 0).translated (0, lower));
    CHECK (screen.startOf (clip) == 8.0);
    CHECK (screen.playlist.getDragReadout() == "Starts at 3.1.1");
    mouse.release();
    CHECK (screen.playlist.getDragReadout().isEmpty());

    // Its end dragged out to bar 5: two bars long.
    mouse.pause();
    const auto edge = screen.boundsOf (clip);
    REQUIRE_FALSE (edge.isEmpty());
    const auto grab = juce::Point<int> (edge.getRight() - 2, edge.getBottom() - 4);
    mouse.press (grab);
    mouse.dragTo (grab.translated (3, 0));
    mouse.dragTo ({ screen.at (16.0, 0).x, grab.y });
    CHECK (screen.playlist.getDragReadout() == "Ends at 5.1.1, 2 bars long");
    mouse.release();
    CHECK (screen.playlist.getDragReadout().isEmpty());
}

TEST_CASE ("A drag past the playlist's edge scrolls as far in the same time however often the mouse reports")
{
    // Known bug 15 until 2026-11-11: each drag counted as at least a 60th of a second, so a mouse that reports a thousand times
    // a second, moving, scrolled sixteen times as fast as one held still, whose drags JUCE repeats every 16 ms.
    const auto pixelsScrolled = [] (int millisecondsApart)
    {
        PlaylistScreen screen;
        screen.addClip (0, 0.0, 4.0);
        phazite::test::dispatchPendingMessages();
        phazite::test::Mouse mouse (*screen.lanes);
        mouse.setEventInterval (millisecondsApart);
        const auto start = screen.at (1.0, 0).translated (0, phazite::ui::TrackHeight::standard / 4); // the lower half moves it
        mouse.press (start);

        // Half a second past the right edge, a pixel back and forth, so that each report is a move.
        for (auto elapsed = 0; elapsed < 500; elapsed += millisecondsApart)
            mouse.dragTo ({ screen.lanesArea().getWidth() + 20 + (elapsed / millisecondsApart) % 2, start.y });

        mouse.release();
        const auto& view = screen.playlist.getView();
        return view.start.inBeats() * view.pixelsPerBeat;
    };

    const auto still = pixelsScrolled (16), moving = pixelsScrolled (1);
    CHECK (still > 100.0);
    CHECK (moving == doctest::Approx (still).epsilon (0.1));
}

TEST_CASE ("A clip dragged past the playlist's edge shows where the song has it at the next frame, before the playlist hears of it")
{
    // Past the edge, the view scrolls first, laying the clips out where the song had them, then the drag moves the clip, which
    // the playlist hears of a message later: a frame painted between showed the clip a scroll's step from the pointer
    // (2026-11-11). The display's frame comes first (TimelineEditor::frameComing()).
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 0.0, 4.0);
    phazite::test::dispatchPendingMessages();
    phazite::test::Mouse mouse (*screen.lanes);
    const auto start = screen.at (1.0, 0).translated (0, phazite::ui::TrackHeight::standard / 4); // the lower half moves it
    mouse.press (start);
    mouse.dragTo ({ screen.lanesArea().getWidth() - 4, start.y }); // into the margin along the edge, which scrolls
    REQUIRE (screen.playlist.getView().start.inBeats() > 0.0);     // it scrolled
    REQUIRE (screen.startOf (clip) > 0.0);                         // and moved the clip, which shows still

    screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0);
    CHECK (screen.boundsOf (clip).getX() == screen.playlist.getView().beatToColumn (beats (screen.startOf (clip))));
    mouse.release();
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
        return phazite::getAutomationPositions (screen.context, volume, { at, at }, 1).front();
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
    const auto notch = (float) phazite::ui::wheelNotch; // as JUCE reports one

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

TEST_CASE ("An empty song's playlist says over its lanes how to start, and a double-click there still adds a track")
{
    PlaylistScreen screen;
    juce::Label* hint = nullptr;

    for (auto* label : phazite::test::findAll<juce::Label> (screen.playlist))
        if (label->getText() == "This song is empty")
            hint = label;

    REQUIRE (hint != nullptr);
    CHECK (! hint->isVisible()); // a song with tracks has none

    phazite::deleteTracks (screen.getSong(), screen.tracks, &screen.selection);
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::getTrackIDs (screen.getSong()).empty());
    CHECK (hint->isShowing());
    CHECK (screen.lanesArea().contains (hint->getBounds()));

    // It takes no clicks: the lanes under it do.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.doubleClick (screen.lanes->getLocalPoint (&screen.playlist, hint->getBounds().getCentre()));
    CHECK (phazite::getTrackIDs (screen.getSong()).size() == 1);
    CHECK (! hint->isVisible());
}

namespace
{
// How many pixels of an area differ between two pictures of the same window.
int countChanged (const juce::Image& before, const juce::Image& after, juce::Rectangle<int> area)
{
    auto changed = 0;
    area = area.getIntersection (before.getBounds()).getIntersection (after.getBounds());

    for (int y = area.getY(); y < area.getBottom(); ++y)
        for (int x = area.getX(); x < area.getRight(); ++x)
            changed += before.getPixelAt (x, y) != after.getPixelAt (x, y) ? 1 : 0;

    return changed;
}
} // namespace

TEST_CASE (
    "The playlist paints a song's sections above its tracks, a clip where it lies and nowhere else, and a selected track's header apart")
{
    PlaylistScreen screen;
    const auto lanes = screen.lanesArea();
    const auto empty = screen.window.paintEverything();

    // The sections, in their strip above the lanes.
    REQUIRE (phazite::findOutline ("pop") != nullptr);
    phazite::applyOutline (screen.getSong(), *phazite::findOutline ("pop"));
    phazite::test::dispatchPendingMessages();
    const auto withSections = screen.window.paintEverything();
    CHECK (countChanged (empty, withSections, { lanes.getX(), 0, lanes.getWidth(), lanes.getY() }) > 0);

    // A clip on the second track, from beat 4 to 12: most of its own area changes, and the first track's row doesn't.
    const auto clip = screen.addClip (1, 4.0, 8.0);
    phazite::test::dispatchPendingMessages();
    const auto withClip = screen.window.paintEverything();
    const auto clipArea = screen.playlist.getLocalArea (screen.lanes, screen.boundsOf (clip)).reduced (2);
    REQUIRE_FALSE (clipArea.isEmpty());
    CHECK (countChanged (withSections, withClip, clipArea) > clipArea.getWidth() * clipArea.getHeight() / 2);
    const auto firstRow =
        screen.playlist.getLocalArea (screen.lanes, juce::Rectangle<int> (0, 0, screen.lanes->getWidth(), screen.heightOf (0)))
            .getIntersection (lanes)
            .reduced (0, 2);
    CHECK (countChanged (withSections, withClip, firstRow) == 0);

    // The third track selected: its header, left of the lanes, changes.
    screen.selection.selectOnly (screen.tracks[2]);
    phazite::test::dispatchPendingMessages();
    const auto selected = screen.window.paintEverything();
    const auto thirdRowTop =
        screen.playlist.getLocalPoint (screen.lanes, juce::Point<int> (0, screen.heightOf (0) + screen.heightOf (1))).y;
    CHECK (countChanged (withClip, selected, { 0, thirdRowTop, lanes.getX(), screen.heightOf (2) }) > 0);
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

    // From the top: the playlist keeps its headers by their tracks, in no order (another on Linux than on Windows).
    std::sort (headers.begin(), headers.end(), [] (const auto* a, const auto* b) { return a->getY() < b->getY(); });
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
    phazite::addSelectionCommands (screen.registry, screen.selection, screen.range, &screen.context);
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
    CHECK (screen.lanesArea().getRight() == screen.playlist.getWidth());

    // The overview of the song along the time bar's top scrolls the time line, as FL Studio's: no scroll bar under the lanes.
    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
        if (! bar->isVertical())
            CHECK_FALSE (bar->isVisible());

    // Tracks that don't fit: the bar shows over the lanes' right edge, thin, and the lanes keep their whole width; it is drawn in
    // front of them, over a see-through track.
    phazite::ensureTracks (screen.getSong(), 30);
    phazite::test::dispatchPendingMessages();
    CHECK (vertical->isVisible());
    CHECK (screen.lanesArea().getRight() == screen.playlist.getWidth());
    CHECK (vertical->getWidth() == phazite::ui::ScrollTint::overlayWidth);
    CHECK (vertical->getRight() == screen.lanesArea().getRight());
    CHECK (vertical->getY() == screen.lanesArea().getY());
    CHECK (screen.playlist.getIndexOfChildComponent (vertical)
           > screen.playlist.getIndexOfChildComponent (screen.lanes->getParentComponent()));
    CHECK_FALSE (vertical->findColour (juce::ScrollBar::trackColourId).isTransparent());
    CHECK (vertical->findColour (juce::ScrollBar::thumbColourId).getAlpha() < 255);

    std::vector<phazite::TrackID> added;

    for (const auto track : phazite::getTrackIDs (screen.getSong()))
        if (std::find (screen.tracks.begin(), screen.tracks.end(), track) == screen.tracks.end())
            added.push_back (track);

    phazite::deleteTracks (screen.getSong(), added, nullptr);

    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (vertical->isVisible());
    CHECK (screen.lanesArea().getRight() == screen.playlist.getWidth());
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
    CHECK (overview->getY() < screen.lanesArea().getY());
    CHECK (overview->getX() == screen.lanesArea().getX());
    CHECK (overview->getRight() == screen.lanesArea().getRight());

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
    // On the whole pixel nearest it at the new zoom (TimelineEditor::setViewStart()).
    CHECK (std::abs (screen.playlist.getView().start.inBeats() - startNow) * screen.playlist.getView().pixelsPerBeat <= 0.5);

    // A press away from the thumb brings that part of the song into view, centred.
    const auto far = juce::Point<int> (overview->getWidth() - 4, overview->getHeight() / 2);
    const auto beatThere =
        timeline->getRangeLimit().getStart() + (double) far.x / overview->getWidth() * timeline->getRangeLimit().getLength();
    mouse.click (far);
    const auto shown = timeline->getCurrentRange();
    CHECK (shown.contains (beatThere));
}

TEST_CASE ("The overview's thumb or its end, dragged far past the strip's end, stops at the time line's end")
{
    // The owner, 2026-11-06: dragged off the screen, the view ran on, each move of the mouse further than the one before,
    // until its beats were so far out that the ruler's loop never ended, and Phazite froze.
    PlaylistScreen screen;
    REQUIRE (screen.addClip (0, 0.0, 4.0).isValid());
    REQUIRE (screen.addClip (2, 32.0, 4.0).isValid());
    phazite::test::dispatchPendingMessages();

    auto* overview = phazite::test::findComponentWithTitle (screen.playlist, "Overview");
    REQUIRE (overview != nullptr);
    juce::ScrollBar* timeline = nullptr;

    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
        if (! bar->isVertical())
            timeline = bar;

    REQUIRE (timeline != nullptr);
    const auto whole = timeline->getRangeLimit();
    const auto xFor = [&] (double beat) { return juce::roundToInt ((beat - whole.getStart()) / whole.getLength() * overview->getWidth()); };

    // The thumb, carried as far as forty strips past the strip's end, a step at a time, as a pointer dragged off the screen
    // goes: the part in view ends where the time line does, which doesn't grow.
    phazite::test::Mouse mouse (*overview);
    const auto view = timeline->getCurrentRange();
    const juce::Point<int> thumb (xFor (view.getStart() + view.getLength() / 2.0), overview->getHeight() / 2);
    mouse.press (thumb);

    for (int step = 1; step <= 40; ++step)
        mouse.dragTo (thumb + juce::Point<int> (step * overview->getWidth(), 0));

    REQUIRE (screen.playlist.getView().start.inBeats() < whole.getEnd());
    mouse.release();
    CHECK (timeline->getCurrentRange().getEnd() == doctest::Approx (whole.getEnd()));
    CHECK (timeline->getRangeLimit().getEnd() == doctest::Approx (whole.getEnd()));

    // Back at the start, its right end carried as far: it zooms out as far as the whole time line, no further.
    mouse.click ({ 1, overview->getHeight() / 2 });
    REQUIRE (timeline->getCurrentRange().getStart() == doctest::Approx (whole.getStart()));
    const juce::Point<int> end (xFor (timeline->getCurrentRange().getEnd()), overview->getHeight() / 2);
    mouse.press (end);

    for (int step = 1; step <= 40; ++step)
        mouse.dragTo (end + juce::Point<int> (step * overview->getWidth(), 0));

    REQUIRE (screen.playlist.getView().pixelsPerBeat >= phazite::ui::TimelineView::minPixelsPerBeat);
    mouse.release();
    CHECK (timeline->getCurrentRange().getEnd() <= whole.getEnd() + 1.0e-6);
    CHECK (timeline->getRangeLimit().getEnd() == doctest::Approx (whole.getEnd()));

    // And the window paints, its rulers too.
    CHECK (screen.window.paintEverything().isValid());
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
    const auto point = screen.lanesArea().getPosition() + screen.at (10.37, 1);
    CHECK (image.getPixelAt (point.x, point.y) == phazite::ui::getPalette (screen.playlist).background);
}

TEST_CASE ("A clip fills its row down to the line under it, and the grid shows through it")
{
    // The owner, 2026-11-07: clips kept a wider gap under them than other DAWs do, and hid the grid, which shows through
    // automation lanes.
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 0.0, 8.0);
    const auto next = screen.addClip (1, 0.0, 8.0);
    phazite::test::dispatchPendingMessages();

    // From its row's top to the line under the row; the next track's clip starts right under that line.
    const auto bounds = screen.boundsOf (clip);
    CHECK (bounds.getY() == 0);
    CHECK (bounds.getBottom() == phazite::ui::TrackHeight::standard - 1);
    CHECK (screen.boundsOf (next).getY() == phazite::ui::TrackHeight::standard);

    // Its track's colour, see-through: over the row as it shows without a clip, and with the bar line at beat 4 through it.
    const auto image = screen.window.paintEverything();
    const auto lanesAt = screen.lanesArea().getPosition();
    const auto xOf = [&screen, lanesAt] (double beat)
    { return lanesAt.x + juce::roundToInt (screen.playlist.getView().beatToX (beats (beat))); };
    const auto y = lanesAt.y + screen.at (0.0, 0).y; // under its name
    const auto row = image.getPixelAt (xOf (10.37), y);
    const auto colour = phazite::getTrackColour (screen.getSong(), screen.tracks[0], 0);
    const auto expected = row.overlaidWith (colour.withAlpha (phazite::ui::ClipTint::body));
    const auto shown = image.getPixelAt (xOf (2.37), y);
    CHECK (std::abs ((int) shown.getRed() - (int) expected.getRed()) <= 2);
    CHECK (std::abs ((int) shown.getGreen() - (int) expected.getGreen()) <= 2);
    CHECK (std::abs ((int) shown.getBlue() - (int) expected.getBlue()) <= 2);
    CHECK (image.getPixelAt (xOf (4.0), y) != shown);
}

TEST_CASE ("Clips side by side on a track have a line of what is behind them between them, not their two edges")
{
    // The owner, 2026-11-07: "the borders of the tracks, when sitting right next to each other, make that part of the border
    // look thick. maybe a 1 px line between would help?"
    PlaylistScreen screen;
    screen.addClip (0, 0.0, 4.0);
    screen.addClip (0, 4.0, 4.0);
    phazite::test::dispatchPendingMessages();

    const auto image = screen.window.paintEverything();
    const auto lanesAt = screen.lanesArea().getPosition();
    const auto y = lanesAt.y + screen.at (0.0, 0).y;
    const auto boundary = lanesAt.x + juce::roundToInt (screen.playlist.getView().beatToX (beats (4.0)));
    const auto colour = phazite::getTrackColour (screen.getSong(), screen.tracks[0], 0);
    const auto row = image.getPixelAt (lanesAt.x + juce::roundToInt (screen.playlist.getView().beatToX (beats (10.37))), y);
    const auto near = [] (juce::Colour a, juce::Colour b)
    {
        return std::abs (a.getRed() - b.getRed()) <= 2 && std::abs (a.getGreen() - b.getGreen()) <= 2
               && std::abs (a.getBlue() - b.getBlue()) <= 2;
    };

    // The first clip's edge, then the row as it shows without a clip, then the second clip's edge.
    CHECK (near (image.getPixelAt (boundary - 2, y), colour));
    CHECK (near (image.getPixelAt (boundary - 1, y), row));
    CHECK (near (image.getPixelAt (boundary, y), colour));
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
    const auto lanesAt = screen.lanesArea().getPosition();
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
    // A song much bigger than the playlist (docs/archive/performance-audit.md, section 8): its clips once all had components, each
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
        mouse.wheel (middle, -(float) phazite::ui::wheelNotch);

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
        mouse.wheel (middle, (float) phazite::ui::wheelNotch);

    screen.playlist.setViewStart (phazite::BeatPosition::fromBeats (2.5));
    phazite::test::dispatchPendingMessages(); // what a jump leaves is made a turn of the message loop later (Playlist.h)
    CHECK (componentOf (first) == nullptr);
    CHECK (componentOf (second) != nullptr);
    screen.playlist.setViewStart ({});
    phazite::test::dispatchPendingMessages();
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
    phazite::test::dispatchPendingMessages();
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
    CHECK (top.getY() == 0);
    CHECK (bottom.getY() == top.getBottom() + 1);
    CHECK (bottom.getBottom() == phazite::ui::TrackHeight::standard - 1);
    CHECK (whole.getHeight() == phazite::ui::TrackHeight::standard - 1);

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
    screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0); // a frame of the display, as the window shows one
    CHECK (viewStart() == doctest::Approx (scrolledTo));

    // Stopped and played again, from where it was moved to last, the view follows it there.
    phazite::playOrStop (screen.getSong());
    screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0); // a frame of the display, as the window shows one
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
    screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0); // a frame of the display, as the window shows one
    CHECK (viewStart() == doctest::Approx (0.0));
    playTo (40.0);
    screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0);
    CHECK (viewStart() == doctest::Approx (32.0).epsilon (0.002));
    playTo (40.5);
    screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0);
    CHECK (viewStart() == doctest::Approx (32.5).epsilon (0.002));

    // Scrolled away while it plays, it stays there.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.wheel ({ 10, 10 }, 1.0f, juce::ModifierKeys::shiftModifier);
    const auto scrolledTo = viewStart();
    playTo (41.0);
    screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0); // a frame of the display, as the window shows one
    CHECK (viewStart() == doctest::Approx (scrolledTo));
    phazite::playOrStop (screen.getSong());
}

TEST_CASE ("Scrolling continuously, the playhead stays on one column while the song moves on by parts of a pixel")
{
    // The owner, 2026-11-07: the playhead jittered while the playlist followed it. With the time line an odd number of
    // pixels wide its middle was half a pixel in, and the playhead's place, rounded, went from one side of it to the other.
    PlaylistScreen screen;
    screen.addClip (0, 0.0, 256.0);

    if (screen.lanesArea().getWidth() % 2 == 0)
        screen.window.setSize (screen.window.getWidth() + 1, screen.window.getHeight());

    REQUIRE (screen.lanesArea().getWidth() % 2 == 1);
    screen.playlist.zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (16.0) });
    screen.playlist.setScrollingContinuously (true);
    auto* playhead = phazite::test::findComponentWithTitle (screen.playlist, "Playhead");
    REQUIRE (playhead != nullptr);
    phazite::playOrStop (screen.getSong());
    REQUIRE (phazite::isSongPlaying (screen.getSong()));

    // Past the middle, at a quarter and three quarters of a pixel: the same column for both, which the view moves under.
    const auto pixelsPerBeat = screen.playlist.getView().pixelsPerBeat;
    const auto pixel = std::floor (40.0 * pixelsPerBeat);
    std::set<int> columns;
    std::set<double> lines;

    for (const auto part : { 0.25, 0.75, 1.25, 1.75 })
    {
        phazite::movePlayhead (screen.getSong(), beats ((pixel + part) / pixelsPerBeat));
        screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0); // a frame of the display, as the window shows one
        REQUIRE (playhead->isVisible());
        columns.insert (playhead->getX());
        lines.insert (screen.playlist.getPlayheadX());
    }

    // Its line on the column too: drawn between pixels, it would wobble while the view moves by whole ones.
    CHECK (columns.size() == 1);
    CHECK (lines.size() == 1);
    CHECK (screen.playlist.getView().start.inBeats() > 0.0);
    phazite::playOrStop (screen.getSong());
}

TEST_CASE ("The playhead's line is drawn where the playhead is, between pixels, so that it glides rather than steps")
{
    // docs/archive/performance-audit.md, section 16 (G2): drawn on whole pixels, at 0.8 of a pixel a frame (the default zoom at 120 BPM
    // on a 60 Hz display) it stood still one frame in five.
    PlaylistScreen screen;
    phazite::test::dispatchPendingMessages();
    auto* playhead = phazite::test::findComponentWithTitle (screen.playlist, "Playhead");
    REQUIRE (playhead != nullptr);
    phazite::ui::LookAndFeel lookAndFeel;
    playhead->setLookAndFeel (&lookAndFeel);
    const auto focus = phazite::ui::getPalette (*playhead).focus;

    // Where the line's colour is, weighed by how much of each pixel it covers, along a row under the marker, in the
    // lanes' pixels.
    const auto paintedCentre = [&]
    {
        juce::Image image (juce::Image::ARGB, playhead->getWidth(), playhead->getHeight(), true, juce::SoftwareImageType());
        {
            juce::Graphics g (image);
            playhead->paintEntireComponent (g, false);
        }

        auto weighed = 0.0, total = 0.0;

        for (int x = 0; x < image.getWidth(); ++x)
            if (const auto pixel = image.getPixelAt (x, image.getHeight() - 4); std::abs (pixel.getRed() - focus.getRed()) < 8)
            {
                weighed += (x + 0.5) * pixel.getFloatAlpha();
                total += pixel.getFloatAlpha();
            }

        return playhead->getX() - screen.lanesArea().getX() + weighed / std::max (1.0e-9, total);
    };

    const auto pixelsPerBeat = screen.playlist.getView().pixelsPerBeat;

    for (const auto x : { 100.25, 100.75, 101.5 })
    {
        screen.playlist.movePlayheadTo (beats (x / pixelsPerBeat));
        CHECK (screen.playlist.getPlayheadX() == doctest::Approx (x));
        CHECK (paintedCentre() == doctest::Approx (x).epsilon (0.0005));
    }

    playhead->setLookAndFeel (nullptr);
}

TEST_CASE ("At each frame while the song plays, the playhead shows the beat heard then, and the frames go into the log once it stops")
{
    // docs/archive/performance-audit.md, section 16 (G3, G6 and option 5): the playhead was read whenever the message thread got to a
    // frame, from the first frame after a fifth of a second; the frames' log tells how the display's frames went.
    PlaylistScreen screen;
    screen.addClip (0, 0.0, 64.0);

    // Phazite's log, as the app writes it, caught for as long as the test lasts, however it ends.
    struct Log : juce::Logger
    {
        Log() { juce::Logger::setCurrentLogger (this); }
        ~Log() override { juce::Logger::setCurrentLogger (nullptr); }
        void logMessage (const juce::String& message) override { lines.add (message); }
        juce::StringArray lines;
    } log;

    // Playing from beat 2, shown from the first frame on. The tests' engine plays no blocks, so the playhead waits where it
    // is put, as if the song had played on to there.
    phazite::movePlayhead (screen.getSong(), beats (2.0));
    phazite::playOrStop (screen.getSong());
    REQUIRE (phazite::isSongPlaying (screen.getSong()));
    auto* session = phazite::findPlayingSession (screen.getSong());
    REQUIRE (session != nullptr);
    session->locate (beats (5.37));
    const auto start = phazite::devices::Clock::getMilliseconds();
    const auto heard = phazite::getHeardPlayheadBeat (screen.context, start);
    CHECK (heard.inBeats() == doctest::Approx (5.37));
    screen.playlist.showFrame (start / 1000.0);
    CHECK (screen.playlist.getPlayheadX() == doctest::Approx (screen.playlist.getView().beatToX (heard)));

    for (int frame = 1; frame < 70; ++frame)
        screen.playlist.showFrame ((start + frame * 1000.0 / 60.0) / 1000.0);

    // Stopped: the next frame puts the playhead back where playback started, and writes the log.
    phazite::playOrStop (screen.getSong());
    screen.playlist.showFrame ((start + 70 * 1000.0 / 60.0) / 1000.0);
    CHECK (screen.playlist.getPlayheadX() == doctest::Approx (screen.playlist.getView().beatToX (beats (2.0))));

    juce::StringArray frameLines;

    for (const auto& line : log.lines)
        if (line.startsWith ("Frames, "))
            frameLines.add (line);

    REQUIRE (frameLines.size() == 1);
    CHECK (frameLines[0].startsWith ("Frames, Playlist (turning pages): 70 in "));
    CHECK (frameLines[0].contains ("one every 16.7 ms"));
}

TEST_CASE ("Scrolling continuously, the song glides under the playhead by parts of a pixel, and a click finds what shows there")
{
    // docs/archive/performance-audit.md, section 16 (option 15): by whole pixels alone, the song stood still one frame in four at the
    // owner's zoom.
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 0.0, 256.0);
    phazite::test::dispatchPendingMessages(); // the playlist shows the clip
    screen.playlist.zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (16.0) });
    screen.playlist.setScrollingContinuously (true);
    phazite::playOrStop (screen.getSong());
    REQUIRE (phazite::isSongPlaying (screen.getSong()));
    auto* session = phazite::findPlayingSession (screen.getSong());
    auto* ruler = phazite::test::findComponentWithTitle (screen.playlist, "Timeline");
    REQUIRE (session != nullptr);
    REQUIRE (ruler != nullptr);

    // Past whole pixels by parts of one: the lanes and the ruler move left by the part of a pixel the song is past the one it
    // reached, always left, and the playhead stays on its column.
    const auto pixelsPerBeat = screen.playlist.getView().pixelsPerBeat;
    const auto pixel = std::floor (40.0 * pixelsPerBeat);
    const auto frame = [&screen] { screen.playlist.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0); };
    std::set<double> lines;

    for (const auto part : { 0.1, 0.3, -0.2, -0.4 })
    {
        session->locate (beats ((pixel + part) / pixelsPerBeat)); // the tests' engine plays no blocks: the song waits there
        frame();
        const auto past = part - std::floor (part);
        CHECK (screen.playlist.getSubPixel() == doctest::Approx (past));
        CHECK (screen.lanes->getTransform().getTranslationX() == doctest::Approx (-past));
        CHECK (ruler->getTransform().getTranslationX() == doctest::Approx (-past));
        lines.insert (screen.playlist.getPlayheadX());
    }

    CHECK (lines.size() == 1);

    // The mouse, moved with them, still finds the clip where it shows.
    REQUIRE_FALSE (juce::exactlyEqual (screen.playlist.getSubPixel(), 0.0f));
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.click (screen.at (44.0, 0));
    mouse.pause();
    CHECK (screen.selection.isSelected (clip));

    // Stopped, on whole pixels again.
    phazite::playOrStop (screen.getSong());
    frame();
    CHECK (screen.playlist.getSubPixel() == 0.0f);
    CHECK (screen.lanes->getTransform().isIdentity());
    CHECK (ruler->getTransform().isIdentity());
}

TEST_CASE ("A view that moves by whole pixels moves the grid's lines and the clips' edges with it, those half a pixel in too")
{
    // The owner, 2026-11-09: scrolling continuously, every other line of the grid wiggled and clips' edges flickered. Each was
    // rounded where it was in view, and one half a pixel in went one way in one frame and the other in the next.
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 40.25, 8.0);
    phazite::test::dispatchPendingMessages();

    // At 54 pixels a beat, a quarter beat is 13.5 pixels: every other line, and both of the clip's edges, half a pixel in.
    screen.playlist.zoom (1.5);
    screen.playlist.zoom (1.5);
    REQUIRE (screen.playlist.getView().pixelsPerBeat == 54.0);
    auto grid = screen.playlist.getGrid();
    grid.adaptive = false;
    grid.fixedLevel = phazite::GridSettings::beatLevel - 2; // a quarter beat
    screen.playlist.setGrid (grid);

    // The columns in a row of the lanes, under the clips' track, that aren't the background: the grid's lines.
    auto& lanes = *screen.lanes;
    const auto background = phazite::ui::getPalette (lanes).background;
    const auto row = screen.at (0.0, 2).y;
    const auto lines = [&lanes, background, row]
    {
        juce::Image image (juce::Image::ARGB, lanes.getWidth(), lanes.getHeight(), true, juce::SoftwareImageType());
        {
            juce::Graphics g (image);
            lanes.paintEntireComponent (g, false);
        }

        std::set<int> columns;

        for (int x = 0; x < image.getWidth(); ++x)
            if (image.getPixelAt (x, row) != background)
                columns.insert (x);

        return columns;
    };

    // The view starting a whole pixel later each time, as scrolling continuously starts it: everything moves a pixel left.
    const auto pixelsPerBeat = screen.playlist.getView().pixelsPerBeat;
    const auto firstPixel = juce::roundToInt (38.0 * pixelsPerBeat);
    std::optional<juce::Rectangle<int>> firstClip;
    std::set<int> before;

    for (int step = 0; step < 16; ++step)
    {
        CAPTURE (step);
        screen.playlist.setViewStart (beats ((firstPixel + step) / pixelsPerBeat));
        const auto bounds = screen.boundsOf (clip);
        REQUIRE_FALSE (bounds.isEmpty());

        if (! firstClip.has_value())
            firstClip = bounds;

        CHECK (bounds == firstClip->translated (-step, 0));

        const auto now = lines();
        CHECK (now.size() > 20);

        if (step > 0)
            for (const auto x : before)
                if (x > 1 && x < lanes.getWidth() - 1)
                    CHECK (now.contains (x - 1));

        before = now;
    }
}

TEST_CASE ("A clip is kept as a picture, which scrolling only moves; one that draws the part of it in view is painted afresh while "
           "the view's edge cuts it")
{
    // docs/archive/performance-audit.md, section 16 (option 16): every clip in view was painted again at each frame the song scrolled.
    PlaylistScreen screen;
    const auto whole = screen.addClip (0, 4.0, 4.0);  // named after its track: no tag
    const auto tagged = screen.addClip (1, 4.0, 4.0); // its name in a tag, which stays in view
    const auto wide = screen.addClip (2, 0.0, 400.0); // larger than a picture should be
    phazite::renameClips (screen.context, { tagged }, "Hook");
    phazite::test::dispatchPendingMessages();

    const auto componentOf = [&screen] (phazite::ClipID id) -> phazite::ui::ClipComponent*
    {
        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (component->getClip() == id)
                return component;

        return nullptr;
    };

    const auto kept = [&componentOf] (phazite::ClipID id)
    {
        auto* component = componentOf (id);
        REQUIRE (component != nullptr);
        return component->getCachedComponentImage() != nullptr;
    };

    CHECK (kept (whole));
    CHECK (kept (tagged));
    CHECK_FALSE (kept (wide));

    // The view's left edge across them: the one with a tag is painted afresh, its tag moving to stay in view; all of it in view
    // again, kept. What the other draws doesn't change with the view, and it stays a picture.
    screen.playlist.setViewStart (beats (6.0));
    CHECK (kept (whole));
    CHECK_FALSE (kept (tagged));
    screen.playlist.setViewStart ({});
    CHECK (kept (tagged));

    // A change to it drops its picture: muted, it shows faint at once.
    const auto lanesAt = screen.lanesArea().getPosition();
    const auto at = lanesAt + screen.at (6.37, 0);
    const auto before = screen.window.paintEverything().getPixelAt (at.x, at.y);
    phazite::toggleClipsMuted (screen.context, { whole });
    phazite::test::dispatchPendingMessages();
    CHECK (screen.window.paintEverything().getPixelAt (at.x, at.y) != before);
}

TEST_CASE ("A clip with a tag stays a picture while the view's top edge cuts it, and draws again with its tag in view as the rows scroll")
{
    // docs/archive/performance-audit.md, section 16 (option 16): only the view's left and right edges, which scroll at every frame of a
    // song playing, stop a clip with a tag being kept; scrolled up or down, it draws its picture again, its tag where it shows.
    PlaylistScreen screen;
    screen.tracks = phazite::ensureTracks (screen.getSong(), 30); // more than fit: the rows scroll
    const auto tagged = screen.addClip (1, 4.0, 4.0);
    phazite::renameClips (screen.context, { tagged }, "Hook");
    phazite::test::dispatchPendingMessages();

    juce::ScrollBar* vertical = nullptr;

    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
        if (bar->isVertical())
            vertical = bar;

    REQUIRE (vertical != nullptr);
    REQUIRE (vertical->isVisible());

    const auto kept = [&screen, tagged]
    {
        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (component->getClip() == tagged)
                return component->getCachedComponentImage() != nullptr;

        return false;
    };

    // Drawn as a picture all in view; then scrolled down by its track's row and 10 pixels more, its top out of view.
    CHECK (kept());
    screen.window.paintEverything();
    vertical->setCurrentRangeStart (screen.heightOf (0) + 10.0, juce::sendNotificationSync);
    const auto shown = screen.boundsOf (tagged);
    REQUIRE (shown.getY() == -10);
    CHECK (kept());

    // Its tag is at the top of what shows of it, in the clip's color: not 10 pixels higher, where its picture had it.
    const auto lanesAt = screen.lanesArea().getPosition();
    const auto tag = screen.window.paintEverything().getPixelAt (lanesAt.x + shown.getX() + phazite::ui::ClipLabelTint::margin + 2,
                                                                 lanesAt.y + phazite::ui::ClipLabelTint::margin
                                                                     + phazite::ui::ClipLabelTint::height / 2);
    const auto colour = phazite::getTrackColour (screen.getSong(), screen.tracks[1], 1);
    CHECK (std::abs ((int) tag.getRed() - (int) colour.getRed()) <= 2);
    CHECK (std::abs ((int) tag.getGreen() - (int) colour.getGreen()) <= 2);
    CHECK (std::abs ((int) tag.getBlue() - (int) colour.getBlue()) <= 2);
}

TEST_CASE ("A clip too long to keep as a picture draws its notes where they are as the view moves along it, muted too")
{
    // docs/archive/performance-audit.md, section 16: such a clip is painted again at each frame the song scrolls, and draws only the
    // notes where it is being painted. Muted, it paints in a layer of its own, faint.
    for (const auto muted : { false, true })
    {
        CAPTURE (muted);
        PlaylistScreen screen;
        const auto clip = screen.addClip (0, 0.0, 400.0); // larger than a picture should be

        if (muted)
            phazite::toggleClipsMuted (screen.context, { clip });

        phazite::test::dispatchPendingMessages();
        screen.playlist.setViewStart (beats (200.0));

        const auto lanesAt = screen.lanesArea().getPosition();
        const auto columnAt = [&screen, lanesAt] (const juce::Image& image, double beat)
        {
            const auto x = lanesAt.x + juce::roundToInt (screen.playlist.getView().beatToX (beats (beat)));
            std::vector<juce::Colour> column;

            for (int y = 0; y < screen.heightOf (0); ++y)
                column.push_back (image.getPixelAt (x, lanesAt.y + y));

            return column;
        };

        const auto before = screen.window.paintEverything();

        // A note in each bar, for its first two beats.
        for (int bar = 0; bar < 100; ++bar)
            phazite::test::addMidiNote (screen.getSong(), clip, 60, phazite::test::at (bar * 4.0), phazite::test::lasting (2.0));

        phazite::test::dispatchPendingMessages();
        const auto after = screen.window.paintEverything();
        CHECK (columnAt (after, 200.37) != columnAt (before, 200.37)); // the 51st bar's note
        CHECK (columnAt (after, 203.37) == columnAt (before, 203.37)); // none
    }
}

TEST_CASE ("The Smart Tool finds where two audio clips meet as the mouse moves, and follows the clips and the zoom")
{
    // docs/archive/performance-audit.md, section 16: each move of the mouse asked the model where the track's audio clips meet, which
    // took a frame's time over a track of a dozen, and the song stuttered as the mouse moved. The playlist keeps them now.
    PlaylistScreen screen;
    phazite::test::TemporaryFolder temp;
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 4.0); // 8 beats
    const auto imported = phazite::importFiles (screen.context, { file.getFullPathName() }, screen.tracks[0], beats (0.0));
    REQUIRE (imported.size() == 1);
    const auto halves = phazite::splitClips (screen.context, imported, beats (4.0));
    REQUIRE (halves.size() == 1);
    phazite::test::dispatchPendingMessages();

    // In the lower third of the row, at the boundary: there, the Smart Tool draws a crossfade.
    phazite::test::Mouse mouse (*screen.lanes);
    const auto crossfadesAt = [&screen, &mouse] (double beat)
    {
        mouse.moveTo ({ juce::roundToInt (screen.playlist.getView().beatToX (beats (beat))), screen.heightOf (0) - 8 });
        return screen.lanes->getDescription().startsWith ("Drag to crossfade");
    };

    CHECK (crossfadesAt (4.0));
    CHECK_FALSE (crossfadesAt (2.0));

    // Zoomed in, where the boundary shows now.
    screen.playlist.zoomTo ({ beats (2.0), beats (6.0) });
    CHECK (crossfadesAt (4.0));
    CHECK_FALSE (crossfadesAt (3.0));

    // The second half moved away: they no longer meet.
    phazite::moveClip (screen.context, halves[0], beats (6.0), screen.tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (crossfadesAt (4.0));
    CHECK_FALSE (crossfadesAt (6.0)); // the first ends at 4, two beats before it
}

TEST_CASE ("An audio clip's waveform stays while its clip leaves the view, and lets go of what it drew")
{
    // The app's waveforms keep what they drew, as large as the part of the clip in view (src/app/ClipWaveforms.cpp). The
    // playlist keeps a clip's waveform, so that its levels don't load again as it comes back into view, and what it drew only
    // while it shows, so that a long song's don't add up.
    struct Waveform : phazite::ui::ClipWaveform
    {
        void paint (juce::Graphics&, juce::Rectangle<int>, int) override { ++paints; }
        void paintSpan (juce::Graphics&, juce::Rectangle<int>, phazite::BeatRange) override {}
        void forgetDrawing() override { ++forgotten; }
        int paints = 0, forgotten = 0;
    };

    std::vector<Waveform*> made;

    struct NoMaker
    {
        ~NoMaker() { phazite::ui::setClipWaveformMaker ({}); } // however the test ends, before what it made goes
    } noMaker;

    phazite::ui::setClipWaveformMaker (
        [&made] (phazite::ProjectContext&, phazite::ClipID, std::function<void()>) -> std::unique_ptr<phazite::ui::ClipWaveform>
        {
            auto waveform = std::make_unique<Waveform>();
            made.push_back (waveform.get());
            return waveform;
        });

    PlaylistScreen screen;
    phazite::test::TemporaryFolder temp;
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 2.0); // 4 beats
    REQUIRE (phazite::importFiles (screen.context, { file.getFullPathName() }, screen.tracks[0], beats (0.0)).size() == 1);
    phazite::test::dispatchPendingMessages();
    screen.window.paintEverything();
    REQUIRE (made.size() == 1);
    CHECK (made[0]->paints > 0);
    CHECK (made[0]->forgotten == 0);

    // Scrolled past it, its waveform forgets what it drew; back in view, the same waveform draws it again.
    screen.playlist.setViewStart (beats (64.0));
    CHECK (made[0]->forgotten == 1);
    const auto paints = made[0]->paints;
    screen.playlist.setViewStart ({});
    screen.window.paintEverything();
    CHECK (made.size() == 1);
    CHECK (made[0]->paints > paints);
    CHECK (made[0]->forgotten == 1);
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

TEST_CASE ("While recording, a press in the ruler takes the playhead there once and recording goes on; a drag takes it no further")
{
    // Each move of the playhead ends the take where it is and starts another there (phazite::movePlayhead()): the press is
    // one move, and a drag after it would be one at every line of the grid.
    PlaylistScreen screen;
    phazite::test::EnginePlayback playback (phazite::test::EnginePlayback::Input::loopback);
    auto& song = screen.getSong();
    phazite::setTrackInput (screen.context, screen.tracks[1], "1");
    phazite::setInputMonitoring (screen.context, screen.tracks[1], phazite::InputMonitoring::never);
    phazite::setArmed (screen.context, screen.tracks[1], true);
    REQUIRE (phazite::isArmed (screen.context, screen.tracks[1]));

    auto* ruler = dynamic_cast<phazite::ui::TimelineRuler*> (phazite::test::findComponentWithTitle (screen.playlist, "Timeline"));
    REQUIRE (ruler != nullptr);
    const auto y = ruler->getHeight() - 4;
    const auto x = [&screen] (double beat) { return juce::roundToInt (screen.playlist.getView().beatToX (beats (beat))); };

    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    playback.play (0.5); // a beat at 120 BPM

    phazite::test::Mouse mouse (*ruler);
    mouse.press ({ x (8.0), y });
    CHECK (phazite::isRecording (song));
    CHECK (phazite::isSongPlaying (song));
    CHECK (phazite::getPlayStartBeat (song).inBeats() == doctest::Approx (8.0));
    mouse.dragTo ({ x (10.0), y });
    mouse.dragTo ({ x (12.0), y });
    mouse.release();
    CHECK (phazite::getPlayStartBeat (song).inBeats() == doctest::Approx (8.0));
    CHECK (phazite::isRecording (song));

    playback.play (0.5);
    phazite::stopRecording (song);
    phazite::test::dispatchPendingMessages();

    // What came in before the press, and what came in after it from where the press put the playhead.
    const auto starts = screen.getClipStarts (1);
    REQUIRE (starts.size() == 2);
    CHECK (starts[0] == doctest::Approx (0.0));
    CHECK (starts[1] == doctest::Approx (8.0));
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

    // Its top is the marker's, at the foot of the ruler (where it shows, through its window): nothing of it above, in the bar
    // numbers.
    const auto rulerArea = screen.playlist.getLocalArea (ruler, ruler->getLocalBounds());
    CHECK (playhead->getY() == rulerArea.getBottom() - phazite::ui::PlayheadTint::markerHeight);
    CHECK (playhead->getBottom() > rulerArea.getBottom());
    CHECK (playhead->getX() + playhead->getWidth() / 2 == rulerArea.getX() + juce::roundToInt (screen.playlist.getView().beatToX ({})));

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

TEST_CASE ("The ruler is quiet: a numbered bar's tick reaches its lane, short ticks for bars and beats far enough apart, none finer")
{
    // The owner, 2026-11-06: "clean up the timeline rulers to be less busy and fit well with our design".
    PlaylistScreen screen;
    phazite::test::dispatchPendingMessages();
    auto* found = dynamic_cast<phazite::ui::TimelineRuler*> (phazite::test::findComponentWithTitle (screen.playlist, "Timeline"));
    REQUIRE (found != nullptr);
    auto& ruler = *found;
    phazite::ui::LookAndFeel lookAndFeel;
    ruler.setLookAndFeel (&lookAndFeel);
    const auto background = phazite::ui::getPalette (ruler).surfaceRaised;
    const auto foot = ruler.getHeight() - 1;

    const auto paintRuler = [&ruler]
    {
        juce::Image image (juce::Image::ARGB, ruler.getWidth(), ruler.getHeight(), true, juce::SoftwareImageType());
        juce::Graphics g (image);
        ruler.paintEntireComponent (g, false);
        return image;
    };

    // The columns a row has ticks in: just above the line along the ruler's foot, under the numbers.
    const auto ticksIn = [&background] (const juce::Image& image, int y)
    {
        std::vector<int> columns;

        for (int x = 0; x < image.getWidth(); ++x)
            if (image.getPixelAt (x, y) != background)
                columns.push_back (x);

        return columns;
    };

    // Two bars across, after the song's loop (its first bar): ticks at its bars and beats only, though eighths would be far
    // enough apart to tick, and a bar's number by a tick that reaches up to the loop's lane, a beat's short.
    screen.playlist.zoomTo ({ phazite::BeatPosition::fromBeats (8.0), phazite::BeatPosition::fromBeats (16.0) });
    auto image = paintRuler();
    const auto beatWidth = screen.playlist.getView().pixelsPerBeat;
    REQUIRE (beatWidth / 8.0 >= phazite::ui::RulerTint::tickSpacing);
    const auto ticks = ticksIn (image, foot - 1);
    CHECK (ticks.size() >= 8);
    CHECK (ticks.size() <= 9);
    const auto xOf = [&screen] (double beat)
    { return juce::roundToInt (screen.playlist.getView().beatToX (phazite::BeatPosition::fromBeats (beat))); };
    CHECK (image.getPixelAt (xOf (12.0), phazite::ui::LoopTint::laneHeight + 1) != background);
    CHECK (image.getPixelAt (xOf (13.0), foot - phazite::ui::RulerTint::beatTick - 1) == background);

    // Zoomed far out, the ticks are as far apart as they need to be, wherever the view is.
    screen.playlist.zoomTo ({ phazite::BeatPosition::fromBeats (64.0), phazite::BeatPosition::fromBeats (64.0 + 2048.0) });
    image = paintRuler();
    const auto farTicks = ticksIn (image, foot - 1);
    REQUIRE (farTicks.size() >= 2);

    for (size_t i = 1; i < farTicks.size(); ++i)
        CHECK (farTicks[i] - farTicks[i - 1] >= phazite::ui::RulerTint::tickSpacing - 1.0);

    // The loop's lane is the ruler's colour, but washed under the pointer, where a drag loops.
    screen.playlist.zoomTo ({ phazite::BeatPosition::fromBeats (8.0), phazite::BeatPosition::fromBeats (16.0) });
    const juce::Point<int> inLane (xOf (10.5), phazite::ui::LoopTint::laneHeight / 2);
    CHECK (paintRuler().getPixelAt (inLane.x, inLane.y) == background);
    phazite::test::Mouse mouse (ruler);
    mouse.moveTo (inLane);
    CHECK (ruler.isLoopLaneLit());
    CHECK (paintRuler().getPixelAt (inLane.x, inLane.y) != background);
    mouse.moveTo ({ inLane.x, ruler.getHeight() + 40 });
    CHECK_FALSE (ruler.isLoopLaneLit());
    ruler.setLookAndFeel (nullptr);
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
    const auto id =
        phazite::importFile (screen.getSong(), screen.tracks[0], phazite::test::writeClicks (temp.folder.getChildFile ("Loop"), 16, 120.0),
                             beats (0.0)); // long enough to see it under way
    REQUIRE (phazite::isAudioClip (screen.getSong(), id));
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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&shownRendering] { return shownRendering().value_or (std::nullopt).has_value(); }));
    const auto progress = *shownRendering().value_or (std::nullopt);
    CHECK (progress >= 0.0);
    CHECK (progress < 1.0);
    CHECK (phazite::test::dispatchMessagesUntil ([&shownRendering] { return ! shownRendering().value_or (0.0).has_value(); }));
}

TEST_CASE (
    "An audio clip whose render fails says so in place of waiting, and the render is tried again only once its file or its folder changes")
{
    // A cache folder that can't be written: where it should be, a file. The render failed, was taken off the list and
    // queued again, for ever (known bug 86).
    PlaylistScreen screen;
    phazite::test::TemporaryFolder temp;
    const auto cache = temp.folder.getChildFile ("Cache");
    REQUIRE (cache.replaceWithText ("not a folder"));
    screen.getSong().setCacheFolder (cache);

    const auto source = phazite::test::writeClicks (temp.folder.getChildFile ("Loop"), 8, 120.0);
    const auto id = phazite::importFile (screen.getSong(), screen.tracks[0], source, beats (0.0));
    REQUIRE (phazite::isAudioClip (screen.getSong(), id));
    phazite::test::dispatchPendingMessages();
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;

    const auto findComponent = [&screen, &ticker, id]() -> phazite::ui::ClipComponent*
    {
        ticker->tickNow();

        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (component->getClip() == id)
                return component;

        return nullptr;
    };

    const auto stateOf = [&screen, id] { return phazite::getAudioClip (screen.context, id).value().state; };

    // Slower, it needs a render, which fails: the clip says so, and nothing is being made any more.
    phazite::setTempo (screen.getSong(), 100.0);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&findComponent]
        {
            auto* component = findComponent();
            return component != nullptr && component->hasRenderFailed();
        }));
    CHECK_FALSE (findComponent()->getRendering().has_value());
    CHECK (stateOf() == phazite::AudioState::failed);
    CHECK (phazite::getClipsWhoseRenderFailed (screen.context) == std::vector { id });

    // The audio clip editor shows it too, and sees it come good below: it looked again only while the audio was being
    // prepared, and said "couldn't be prepared" for good (a reviewer, 2026-11-14).
    phazite::ui::AudioClipEditor editor { screen.commands, screen.context };
    phazite::test::TestWindow editorWindow { editor, 800, 300 };
    editor.showClip (id);
    CHECK (editor.getShownState() == phazite::AudioState::failed);

    // While nothing changes, it looks again a few times a second and repaints nothing: it fitted its view and repainted its
    // waveform at each look (a reviewer, 2026-11-14). Over a second of its looks goes by without a repaint, once the
    // file's picture has loaded: in pieces, each repainting it, as far apart as the machine is busy (CI's windows-debug saw
    // one after 400 ms without any, 2026-11-14), so the wait is for that second, however long the pieces take.
    CHECK (phazite::test::dispatchMessagesUntil (
        [&editorWindow, &editor]
        {
            editorWindow.takeRepaintedArea();
            phazite::test::dispatchMessagesUntil ([] { return false; }, 1100);
            return editorWindow.takeRepaintedArea().isEmpty() || editor.getShownState() != phazite::AudioState::failed;
        }));
    CHECK (editor.getShownState() == phazite::AudioState::failed);

    // It isn't tried again while nothing changed: the check the renders make every two seconds while one has failed, made
    // at once, starts nothing. (Rounds of messages alone, which pass in microseconds, never reached that check.)
    auto* renders = phazite::getClipRenders (screen.context);
    REQUIRE (renders != nullptr);
    const auto started = renders->getRendersStarted();

    for (int round = 0; round < 3; ++round)
    {
        renders->checkFailedAgain(); // its folder looked at in the background
        REQUIRE (phazite::test::dispatchMessagesUntil ([renders] { return ! renders->isCheckingFolders(); }));
        REQUIRE (renders->getRendersStarted() == started);
        REQUIRE (stateOf() == phazite::AudioState::failed);
    }

    // The clip deleted, it isn't listed as failed at once, before the renders look again; back by Undo, it is again.
    {
        const phazite::UndoStep step (screen.getSong(), "Delete Clip");
        phazite::deleteClips (screen.getSong(), { id }, false);
    }

    CHECK (phazite::getClipsWhoseRenderFailed (screen.context).empty());
    phazite::undo (screen.getSong());
    CHECK (phazite::getClipsWhoseRenderFailed (screen.context) == std::vector { id });

    // The cache can be written now, the file unchanged: the next check tries it again (it was never tried again while the
    // file stayed the same), and the render is made and played, and the clip no longer says so.
    REQUIRE (cache.deleteFile());
    REQUIRE (cache.createDirectory());
    renders->checkFailedAgain();
    REQUIRE (phazite::test::dispatchMessagesUntil ([renders, started] { return renders->getRendersStarted() == started + 1; }));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&findComponent, &stateOf]
        {
            auto* component = findComponent();
            return component != nullptr && ! component->hasRenderFailed() && stateOf() == phazite::AudioState::ready;
        }));
    CHECK (phazite::getClipsWhoseRenderFailed (screen.context).empty());
    CHECK (phazite::test::dispatchMessagesUntil ([&editor] { return editor.getShownState() == phazite::AudioState::ready; }));
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

    const auto isLabelShown = [&screen] (const juce::String& text)
    {
        const auto labels = phazite::test::findAll<juce::Label> (screen.playlist);
        return std::any_of (labels.begin(), labels.end(),
                            [&text] (juce::Label* label) { return label->isVisible() && label->getText() == text; });
    };

    auto* arm = findArm (0);
    REQUIRE (arm != nullptr);
    CHECK (arm->isVisible());

    // A track with an instrument records what is played on MIDI: its switch is lit on the track live MIDI plays, as Ableton
    // Live arms the selected MIDI track by itself, and a click sends live MIDI there, or stops it recording there.
    phazite::setInstrument (screen.getSong(), screen.tracks[1], phazite::makeOwnDeviceState (screen.getSong(), "phaziteSynth"));
    phazite::test::dispatchPendingMessages();
    auto* midiArm = findArm (1);
    REQUIRE (midiArm != nullptr);
    CHECK (midiArm->isVisible());
    CHECK (midiArm->getComponentID() == juce::String ("track.armMidi")); // the recording lesson rings the audio one
    CHECK_FALSE (midiArm->getToggleState());

    phazite::test::Mouse (*midiArm).click (midiArm->getLocalBounds().getCentre());
    CHECK (phazite::getLiveMidiTrack (screen.context) == screen.tracks[1]);
    CHECK (phazite::isMidiArmed (screen.context, screen.tracks[1]));
    CHECK (midiArm->getToggleState());
    CHECK (isLabelShown ("MIDI"));
    CHECK (phazite::describeRecordTargets (screen.context) == phazite::getTrackName (screen.getSong(), screen.tracks[1]) + " (MIDI)");

    phazite::test::Mouse (*midiArm).click (midiArm->getLocalBounds().getCentre());
    CHECK (phazite::getLiveMidiTrack (screen.context) == screen.tracks[1]); // it still plays
    CHECK_FALSE (phazite::isMidiArmed (screen.context, screen.tracks[1]));
    CHECK_FALSE (midiArm->getToggleState());
    CHECK_FALSE (isLabelShown ("MIDI"));

    // With an input, a click arms the track; its header says what it records, and the click is no undo step.
    phazite::setTrackInput (screen.context, screen.tracks[0], "1");
    phazite::test::dispatchPendingMessages();
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::Mouse mouse (*arm);
    mouse.click (arm->getLocalBounds().getCentre());
    CHECK (phazite::isArmed (screen.context, screen.tracks[0]));
    CHECK (arm->getToggleState());
    CHECK_FALSE (screen.getSong().getUndoManager().canUndo());
    CHECK (isLabelShown ("In 1"));

    // Armed elsewhere (the Arm Track command, the mixer), it shows at the displays' next tick.
    phazite::setArmed (screen.context, screen.tracks[0], false);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK_FALSE (arm->getToggleState());
}

TEST_CASE ("While recording, what comes in shows on its track as a clip growing with the playhead, until the take's clip is there")
{
    // As Ableton Live draws a clip as it records (model/transport/RecordingModel.h: getRecordingsInProgress()).
    PlaylistScreen screen;
    phazite::test::EnginePlayback playback (phazite::test::EnginePlayback::Input::loopback);
    auto& song = screen.getSong();
    phazite::setTrackInput (screen.context, screen.tracks[1], "1");
    phazite::setInputMonitoring (screen.context, screen.tracks[1], phazite::InputMonitoring::never);
    phazite::setArmed (screen.context, screen.tracks[1], true);
    REQUIRE (phazite::isArmed (screen.context, screen.tracks[1]));

    const auto findRecording = [&screen]() -> phazite::ui::ClipComponent*
    {
        for (auto* clip : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (clip->isRecording())
                return clip;

        return nullptr;
    };

    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();
    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    playback.play (1.0); // two beats at 120 BPM
    ticker.tickNow();

    // On the armed track's row, from where recording started to the playhead, saying that it records.
    auto* recording = findRecording();
    REQUIRE (recording != nullptr);
    CHECK (recording->isVisible());
    CHECK (recording->getLabel() == "Recording");
    const auto shown = screen.lanes->getLocalArea (recording, recording->getLocalBounds());
    CHECK (shown.getX() == screen.at (0.0, 1).x);
    CHECK (std::abs (shown.getRight() - screen.at (2.0, 1).x) <= 1);
    CHECK (shown.contains (screen.at (1.0, 1)));

    // It grows as the song plays on.
    playback.play (1.0);
    ticker.tickNow();
    REQUIRE (findRecording() == recording);
    CHECK (std::abs (screen.lanes->getLocalArea (recording, recording->getLocalBounds()).getRight() - screen.at (4.0, 1).x) <= 1);

    // A part of a pixel on, what it shows is over the beats of its whole columns still: spread over the beats it had recorded,
    // an input's levels went from one column to the next at every frame as it grew (2026-11-11).
    playback.play (0.013);
    ticker.tickNow();
    REQUIRE (findRecording() == recording);
    CHECK (recording->getInfo().range.getLength().inBeats() * screen.playlist.getView().pixelsPerBeat
           == doctest::Approx ((double) recording->getWidth()));

    // Stopped: it goes, and the clip the take made is where it was.
    phazite::stopRecording (song);
    phazite::test::dispatchPendingMessages();
    ticker.tickNow();
    CHECK (findRecording() == nullptr);
    const auto clips = phazite::getClips (song, screen.tracks[1]);
    REQUIRE (clips.size() == 1);
    CHECK (screen.boundsOf (clips[0]).getX() == shown.getX());
}

TEST_CASE ("A take waiting for a drag's end and the take recording after it on the same track each draw their own levels")
{
    // Known bug 176's review: the levels of a track's recording came from the latest take that showed on it, so a take
    // stopped during a drag, still waiting to be placed, drew the next take's levels over its own stretch (none there). The
    // metronome's clicks, on beats 0 and 1, come back into the armed track's input.
    PlaylistScreen screen;
    phazite::test::EnginePlayback playback (phazite::test::EnginePlayback::Input::loopback);
    auto& song = screen.getSong();
    const auto track = screen.tracks[1];
    phazite::setMetronomeOn (song, true);
    phazite::setTrackInput (screen.context, track, "1");
    phazite::setInputMonitoring (screen.context, track, phazite::InputMonitoring::never);
    phazite::setArmed (screen.context, track, true);
    REQUIRE (phazite::isArmed (screen.context, track));
    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();

    phazite::startRecording (song);
    playback.play (1.0); // beats 0 to 2

    // Stopped while a fader is held: the take waits; then another, from beat 4.
    auto volume = phazite::getVolume (screen.context, screen.tracks[0]);
    volume.beginGesture();
    volume.setValue (0.3f);
    phazite::stopRecording (song);
    phazite::movePlayhead (song, phazite::BeatPosition::fromBeats (4.0));
    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));
    playback.play (1.0); // beats 4 to 6

    const auto loudest = [] (const std::vector<float>& levels) { return *std::max_element (levels.begin(), levels.end()); };
    const auto span = [] (double from, double to)
    { return phazite::BeatRange (phazite::BeatPosition::fromBeats (from), phazite::BeatPosition::fromBeats (to)); };
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return loudest (phazite::getRecordingLevels (screen.context, track, span (4.0, 6.0), 64)) > 0.0f; }));
    CHECK (loudest (phazite::getRecordingLevels (screen.context, track, span (0.0, 2.0), 64)) > 0.0f);

    // On the screen: both, the waiting one with its clicks drawn.
    ticker.tickNow();
    std::vector<phazite::ui::ClipComponent*> recordings;

    for (auto* clip : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
        if (clip->isRecording())
            recordings.push_back (clip);

    REQUIRE (recordings.size() == 2);
    std::sort (recordings.begin(), recordings.end(),
               [] (const auto* a, const auto* b) { return a->getInfo().range.getStart() < b->getInfo().range.getStart(); });
    auto& waiting = *recordings.front();
    juce::Image painted (juce::Image::ARGB, waiting.getWidth(), waiting.getHeight(), true, juce::SoftwareImageType());
    {
        juce::Graphics g (painted);
        waiting.paintEntireComponent (g, false);
    }

    // Where nothing came in (half a beat after the click on beat 1), the middle row is the clip's own colour; at the click,
    // the waveform crosses it.
    const auto middle = waiting.getHeight() / 2;
    const auto pixelsPerBeat = screen.playlist.getView().pixelsPerBeat;
    const auto quiet = painted.getPixelAt ((int) (1.5 * pixelsPerBeat), middle);
    auto drawn = 0;

    for (auto x = (int) (0.95 * pixelsPerBeat); x <= (int) (1.15 * pixelsPerBeat); ++x)
        drawn += painted.getPixelAt (x, middle) != quiet ? 1 : 0;

    CHECK (drawn > 0);

    phazite::stopRecording (song);
    volume.endGesture();
    CHECK (phazite::getClips (song, track).size() == 2);
}

TEST_CASE ("Recording from before a looped range, the part before it shows on its own once round it, this time round beside it")
{
    // Known bug 10 until 2026-11-10: the part before the range was a take of the range's clip. Beats 4 to 8 are looped.
    PlaylistScreen screen;
    phazite::test::EnginePlayback playback (phazite::test::EnginePlayback::Input::loopback);
    auto& song = screen.getSong();
    phazite::setTrackInput (screen.context, screen.tracks[1], "1");
    phazite::setInputMonitoring (screen.context, screen.tracks[1], phazite::InputMonitoring::never);
    phazite::setArmed (screen.context, screen.tracks[1], true);
    REQUIRE (phazite::isArmed (screen.context, screen.tracks[1]));
    phazite::loopBeatRange (song, 4.0, 8.0);

    // The clips that show what is being recorded, left to right, and where each is in the lanes.
    const auto areaOf = [&screen] (juce::Component* clip) { return screen.lanes->getLocalArea (clip, clip->getLocalBounds()); };
    const auto findRecordings = [&screen, &areaOf]
    {
        std::vector<phazite::ui::ClipComponent*> found;

        for (auto* clip : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (clip->isRecording() && clip->isVisible())
                found.push_back (clip);

        std::sort (found.begin(), found.end(), [&areaOf] (auto* a, auto* b) { return areaOf (a).getX() < areaOf (b).getX(); });
        return found;
    };

    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();
    phazite::startRecording (song);
    REQUIRE (phazite::isRecording (song));

    // The first time round: one clip, growing from where recording started.
    playback.play (3.0); // to beat 6
    ticker.tickNow();
    auto shown = findRecordings();
    REQUIRE (shown.size() == 1);
    CHECK (std::abs (areaOf (shown[0]).getX() - screen.at (0.0, 1).x) <= 1);
    CHECK (std::abs (areaOf (shown[0]).getRight() - screen.at (6.0, 1).x) <= 1);

    // Round once: the part before the range, which grows no more, and this time round from the range's start to the playhead.
    playback.play (2.0); // to beat 10, beat 6 the second time round
    ticker.tickNow();
    shown = findRecordings();
    REQUIRE (shown.size() == 2);
    CHECK (std::abs (areaOf (shown[0]).getX() - screen.at (0.0, 1).x) <= 1);
    CHECK (std::abs (areaOf (shown[0]).getRight() - screen.at (4.0, 1).x) <= 1);
    CHECK (shown[0]->getLabel() == "Recording");
    CHECK (std::abs (areaOf (shown[1]).getX() - screen.at (4.0, 1).x) <= 1);
    CHECK (std::abs (areaOf (shown[1]).getRight() - screen.at (6.0, 1).x) <= 1);
    CHECK (shown[1]->getLabel() == "Recording take 2");

    // Just round again, the playhead within the reach that a growing recording's end keeps to it, the part before the range
    // still ends at the range's start.
    const auto pastStart = phazite::ui::RecordingTint::reach / 2.0 / screen.playlist.getView().pixelsPerBeat; // in beats
    playback.play (1.0 + pastStart * 0.5);                                                                    // at 120 BPM
    ticker.tickNow();
    shown = findRecordings();
    REQUIRE (shown.size() == 2);
    CHECK (std::abs (areaOf (shown[0]).getRight() - screen.at (4.0, 1).x) <= 1);
    CHECK (std::abs (areaOf (shown[1]).getRight() - screen.at (4.0 + pastStart, 1).x) <= 1);

    // Stopped: the clips the take made are where they showed.
    phazite::stopRecording (song);
    phazite::test::dispatchPendingMessages();
    ticker.tickNow();
    CHECK (findRecordings().empty());
    const auto clips = phazite::getClips (song, screen.tracks[1]);
    REQUIRE (clips.size() == 2);
    CHECK (std::abs (screen.boundsOf (clips[0]).getRight() - screen.at (4.0, 1).x) <= 1);
    CHECK (std::abs (screen.boundsOf (clips[1]).getX() - screen.at (4.0, 1).x) <= 1);
}

TEST_CASE ("A clip shows a tag with its name only where that isn't its track's or like it, and a link while it is an alias")
{
    // The owner, 2026-11-07: a strip along the top of every clip repeated its track's name, with a "*" after an alias's.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    PlaylistScreen screen;
    auto& song = screen.getSong();
    phazite::setTrackName (song, screen.tracks[0], "Keys");
    const auto clip = screen.addClip (0, 0.0, 8.0);
    const auto aliases = phazite::copyClips (song, { clip }, true);
    REQUIRE (aliases.size() == 1);
    phazite::moveClip (song, aliases[0], beats (16.0), screen.tracks[0]);
    const auto alone = screen.addClip (1, 0.0, 8.0);
    phazite::test::dispatchPendingMessages();

    const auto componentOf = [&screen] (phazite::ClipID id) -> phazite::ui::ClipComponent*
    {
        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (component->getClip() == id)
                return component;

        return nullptr;
    };

    // Where its name's tag goes, at its top left, and its marks', at its top right: whether the tag's colour, opaque, shows
    // there, or the clip see-through. (Inside each tag's edge, beside what is drawn on it.)
    const auto tagShows = [&screen] (const phazite::ui::ClipComponent& shown, bool atRight)
    {
        const auto area = screen.playlist.getLocalArea (&shown, shown.getLocalBounds());
        const auto x = atRight ? area.getRight() - 1 - phazite::ui::ClipLabelTint::margin - 2 * phazite::ui::ClipLabelTint::iconGap
                                     - phazite::ui::ClipLabelTint::iconSize + 1
                               : area.getX() + phazite::ui::ClipLabelTint::margin + 1;
        const auto y = area.getY() + phazite::ui::ClipLabelTint::margin + phazite::ui::ClipLabelTint::height / 2;
        return screen.window.paintEverything().getPixelAt (x, y) == shown.getColour();
    };

    // Called after its track: no name over it, which screen readers still read, as they read that it is an alias. The link
    // shows on both aliases, not on the clip that is none.
    auto* shown = componentOf (clip);
    auto* other = componentOf (alone);
    REQUIRE (shown != nullptr);
    REQUIRE (other != nullptr);
    CHECK (shown->getLabel().isEmpty());
    CHECK (shown->getTitle() == juce::String ("Keys"));
    CHECK (shown->getDescription().contains ("alias"));
    CHECK_FALSE (tagShows (*shown, false));
    CHECK (tagShows (*shown, true));
    CHECK_FALSE (tagShows (*other, true));

    // A name of its own like its track's, but for its capitals and a tempo, as a sample's file name often is: no tag either,
    // though screen readers read it (2026-11-07, the owner: "Shaker" over the track, "Shaker 85bmp" over its clips).
    phazite::renameClips (screen.context, { clip }, "keys_120bpm");
    phazite::test::dispatchPendingMessages();
    CHECK (shown->getLabel().isEmpty());
    CHECK (shown->getTitle() == juce::String ("keys_120bpm"));
    CHECK_FALSE (tagShows (*shown, false));

    // A name of its own: on it and its alias.
    phazite::renameClips (screen.context, { clip }, "Verse");
    phazite::test::dispatchPendingMessages();
    CHECK (shown->getLabel() == juce::String ("Verse"));
    REQUIRE (componentOf (aliases[0]) != nullptr);
    CHECK (componentOf (aliases[0])->getLabel() == juce::String ("Verse"));
    CHECK (tagShows (*shown, false));

    // Made unique, the link goes; called after its track again, so does the name.
    phazite::makeClipsUnique (song, { clip });
    phazite::renameClips (screen.context, { clip }, {});
    phazite::test::dispatchPendingMessages();
    CHECK (shown->getLabel().isEmpty());
    CHECK_FALSE (tagShows (*shown, true));
}

TEST_CASE ("A muted clip is drawn faint, all of it: its row shows through it more than through a clip that plays")
{
    // Known bug 11 until 2026-11-07: the playlist drew a muted clip as any other, though a folder's row and the overview
    // showed it faint.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 0.0, 8.0);
    phazite::test::dispatchPendingMessages();

    // The middle of its row, between the grid's lines, in it and past its end.
    const auto lanesAt = screen.lanesArea().getPosition();
    const auto inClip = lanesAt + screen.at (2.37, 0), pastIt = lanesAt + screen.at (12.37, 0);
    const auto distance = [] (juce::Colour a, juce::Colour b)
    {
        return std::abs ((int) a.getRed() - (int) b.getRed()) + std::abs ((int) a.getGreen() - (int) b.getGreen())
               + std::abs ((int) a.getBlue() - (int) b.getBlue());
    };

    auto image = screen.window.paintEverything();
    const auto row = image.getPixelAt (pastIt.x, pastIt.y);
    const auto playing = image.getPixelAt (inClip.x, inClip.y);

    phazite::toggleClipsMuted (screen.getSong(), { clip });
    phazite::test::dispatchPendingMessages();
    image = screen.window.paintEverything();
    const auto muted = image.getPixelAt (inClip.x, inClip.y);
    CHECK (distance (muted, row) < distance (playing, row) / 2);
    CHECK (muted != row); // faint, not gone

    // Screen readers say so too.
    const auto components = phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist);
    REQUIRE (components.size() == 1);
    CHECK (components.getFirst()->getDescription().contains ("Muted"));
}

TEST_CASE ("A track that isn't heard, muted or kept quiet by another's solo, shows its clips faint, as a muted clip shows")
{
    // The owner, 2026-11-07, as other DAWs dim a muted track's clips.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    PlaylistScreen screen;
    const auto first = screen.addClip (0, 0.0, 8.0);
    screen.addClip (1, 0.0, 8.0);
    phazite::test::dispatchPendingMessages();

    // How far a track's clip is from its row's colour past its end, in the middle of the row, between the grid's lines.
    const auto lanesAt = screen.lanesArea().getPosition();
    const auto contrast = [&screen, lanesAt] (int track)
    {
        const auto image = screen.window.paintEverything();
        const auto inClip = lanesAt + screen.at (2.37, track), pastIt = lanesAt + screen.at (12.37, track);
        const auto a = image.getPixelAt (inClip.x, inClip.y), b = image.getPixelAt (pastIt.x, pastIt.y);
        return std::abs ((int) a.getRed() - (int) b.getRed()) + std::abs ((int) a.getGreen() - (int) b.getGreen())
               + std::abs ((int) a.getBlue() - (int) b.getBlue());
    };

    const std::array playing { contrast (0), contrast (1) }; // each in its track's colour
    REQUIRE (playing[0] > 0);
    REQUIRE (playing[1] > 0);
    const auto isFaint = [&contrast, &playing] (int track)
    {
        const auto shown = contrast (track);
        return shown > 0 && shown < playing[(size_t) track] / 2; // faint, not gone
    };

    const auto clipOf = [&screen] (phazite::ClipID clip) -> phazite::ui::ClipComponent*
    {
        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (component->getClip() == clip)
                return component;

        return nullptr;
    };

    // Muted, its clips are faint, and screen readers say why; unmuted, they are as they were.
    phazite::toggleMute (screen.context, screen.tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK (isFaint (0));
    REQUIRE (clipOf (first) != nullptr);
    CHECK (clipOf (first)->getDescription().contains ("isn't heard"));
    phazite::toggleMute (screen.context, screen.tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK (contrast (0) == playing[0]);
    CHECK_FALSE (clipOf (first)->getDescription().contains ("isn't heard"));

    // The other track soloed: its clips play, and this one's are faint until the solo goes.
    phazite::toggleSolo (screen.context, screen.tracks[1]);
    phazite::test::dispatchPendingMessages();
    CHECK (isFaint (0));
    CHECK (contrast (1) == playing[1]);
    phazite::toggleSolo (screen.context, screen.tracks[1]);
    phazite::test::dispatchPendingMessages();
    CHECK (contrast (0) == playing[0]);
}

TEST_CASE ("With Snap on, a move takes clips by the grid's steps from where they were: a clip off the grid stays as far off it")
{
    // A drag to another track pulled a clip off the grid onto it, and every selected clip with it: the start snapped, not
    // the move.
    PlaylistScreen screen;
    REQUIRE (screen.playlist.getGrid().snap);
    const auto step = screen.playlist.getGridStepInBeats();
    REQUIRE (step > 0.05);
    const auto offGrid = 4.0 + step * 0.3; // as a clip placed with Alt, or a split's half, is
    const auto clip = screen.addClip (0, offGrid, 4.0);
    phazite::test::dispatchPendingMessages();
    phazite::test::Mouse mouse (*screen.lanes);
    const auto lower = phazite::ui::TrackHeight::standard / 4;

    // Down a track, and nothing sideways: on the next track, where it was in time.
    const auto from = screen.at (offGrid + 2.0, 0).translated (0, lower);
    mouse.press (from);
    mouse.dragTo (from.translated (0, 5));
    mouse.dragTo (screen.at (offGrid + 2.0, 1).translated (0, lower));
    mouse.release();
    mouse.pause();
    CHECK (screen.getClipStarts (0).empty());
    REQUIRE (screen.getClipStarts (1).size() == 1);
    CHECK (screen.startOf (clip) == doctest::Approx (offGrid));
    CHECK (phazite::describeUndo (&screen.getSong()) == juce::String ("Undo Move Clip"));
    phazite::test::dispatchPendingMessages(); // the playlist shows the clip on its new track

    // Sideways by a bar: a bar later, as far off the grid as it was.
    const auto grabbed = screen.at (offGrid + 2.0, 1).translated (0, lower);
    mouse.press (grabbed);
    mouse.dragTo (grabbed.translated (5, 0));
    mouse.dragTo (grabbed.translated (screen.at (4.0, 0).x - screen.at (0.0, 0).x, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.startOf (clip) == doctest::Approx (offGrid + 4.0));
}

TEST_CASE ("A Ctrl+click on a clip whose pointer slips copies nothing, and a drag that ends where it began leaves no undo step")
{
    // Ctrl+click toggles a clip, but four pixels of movement began a drag that copied at once: with Snap on the copy stayed
    // on its original, in a sub-row of its own, playing twice.
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 4.0, 4.0);
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    phazite::test::Mouse mouse (*screen.lanes);
    const auto from = screen.at (6.0, 0).translated (0, phazite::ui::TrackHeight::standard / 4);
    auto& selection = screen.context.getSongSelection();
    REQUIRE_FALSE (selection.isSelected (clip));

    // The pointer slips five pixels during a Ctrl+click: the clip is selected, as a Ctrl+click's is; again, and it isn't.
    for (const auto selected : { true, false })
    {
        mouse.press (from, juce::ModifierKeys::commandModifier);
        mouse.dragTo (from.translated (5, 0));
        mouse.release();
        mouse.pause();
        CHECK (screen.getClipStarts (0) == std::vector { 4.0 });
        CHECK (selection.isSelected (clip) == selected);
        CHECK_FALSE (phazite::canUndo (screen.getSong()));
    }

    // A Ctrl+drag makes its copy once it moves it; back where it began, the copy goes, and nothing is left to undo or redo.
    const auto fourBeats = screen.at (4.0, 0).x - screen.at (0.0, 0).x;
    mouse.press (from, juce::ModifierKeys::commandModifier);
    mouse.dragTo (from.translated (5, 0));
    CHECK (screen.getClipStarts (0) == std::vector { 4.0 });
    mouse.dragTo (from.translated (fourBeats, 0));
    CHECK (screen.getClipStarts (0) == std::vector { 4.0, 8.0 });
    mouse.dragTo (from.translated (2, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getClipStarts (0) == std::vector { 4.0 });
    CHECK (selection.isSelected (clip)); // the click
    CHECK_FALSE (phazite::canUndo (screen.getSong()));
    CHECK_FALSE (phazite::canRedo (screen.getSong()));

    // A move there and back leaves no step either.
    mouse.press (from);
    mouse.dragTo (from.translated (fourBeats, 0));
    CHECK (screen.startOf (clip) == 8.0);
    mouse.dragTo (from.translated (1, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.startOf (clip) == 4.0);
    CHECK_FALSE (phazite::canUndo (screen.getSong()));

    // And a Ctrl+drag that ends elsewhere copies, as one step.
    mouse.press (from, juce::ModifierKeys::commandModifier);
    mouse.dragTo (from.translated (5, 0));
    mouse.dragTo (from.translated (2 * fourBeats, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getClipStarts (0) == std::vector { 4.0, 12.0 });
    CHECK (phazite::describeUndo (&screen.getSong()) == juce::String ("Undo Copy Clip"));
    phazite::undo (screen.getSong());
    CHECK (screen.getClipStarts (0) == std::vector { 4.0 });
}

TEST_CASE ("A drag of selected clips that ends where it began keeps them all selected, and a click on one still selects it alone")
{
    // A drag by less than half a grid step moves nothing: taken for the click, it selected the clip pressed alone.
    PlaylistScreen screen;
    const auto first = screen.addClip (0, 4.0, 4.0), second = screen.addClip (0, 12.0, 4.0);
    screen.getSong().getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    phazite::test::Mouse mouse (*screen.lanes);
    const auto lower = phazite::ui::TrackHeight::standard / 4;
    const auto from = screen.at (6.0, 0).translated (0, lower);
    auto& selection = screen.context.getSongSelection();
    mouse.click (from);
    mouse.click (screen.at (14.0, 0).translated (0, lower), juce::ModifierKeys::commandModifier);
    REQUIRE (selection.getClips() == std::vector { first, second });

    // The pointer goes five pixels and the mouse goes up: nothing moved, both stay selected, and no undo step.
    mouse.press (from);
    mouse.dragTo (from.translated (5, 0));
    mouse.release();
    mouse.pause();
    CHECK (selection.getClips() == std::vector { first, second });
    CHECK_FALSE (phazite::canUndo (screen.getSong()));

    // There and back likewise.
    mouse.press (from);
    mouse.dragTo (from.translated (5, 0));
    mouse.dragTo (from.translated (screen.at (4.0, 0).x - screen.at (0.0, 0).x, 0));
    CHECK (screen.startOf (first) == 8.0);
    mouse.dragTo (from.translated (1, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getClipStarts (0) == std::vector { 4.0, 12.0 });
    CHECK (selection.getClips() == std::vector { first, second });
    CHECK_FALSE (phazite::canUndo (screen.getSong()));

    // A Ctrl+press that slips still takes the clip out, as its click does; a click without a drag selects one alone.
    mouse.press (from, juce::ModifierKeys::commandModifier);
    mouse.dragTo (from.translated (5, 0));
    mouse.release();
    mouse.pause();
    CHECK (selection.getClips() == std::vector { second });
    mouse.click (from, juce::ModifierKeys::commandModifier);
    REQUIRE (selection.getClips().size() == 2);
    mouse.click (from);
    CHECK (selection.getClips() == std::vector { first });
}

TEST_CASE ("A sound file dropped on a track's row in the playlist becomes a clip there, from the beat under it, selected, in one undo step")
{
    // As Explorer drops a file: the playlist, a JUCE file drag's target, is asked whether it wants the files, told where they
    // move, and given them, at a place in its own coordinates.
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& context = screen.session.getContext();
    auto& song = *phazite::findSong (context);
    const auto tracks = phazite::ensureTracks (song, 2);
    song.getUndoManager().clearUndoHistory();
    auto& playlist = screen.showView<phazite::ui::Playlist> ("view.playlist");
    auto* lanes = phazite::test::findComponentWithTitle (playlist, "Tracks");
    REQUIRE (lanes != nullptr);

    auto& dropTarget = static_cast<juce::FileDragAndDropTarget&> (playlist); // as JUCE finds it under the files
    const auto sound = phazite::test::writeSound (screen.temp.folder.getChildFile ("Dropped.wav"), 1.0);
    const auto text = screen.temp.folder.getChildFile ("Notes.txt");
    REQUIRE (text.replaceWithText ("not a sound"));
    CHECK (dropTarget.isInterestedInFileDrag ({ sound.getFullPathName() }));
    CHECK_FALSE (dropTarget.isInterestedInFileDrag ({ text.getFullPathName() }));

    // On the second track's row, half a beat after beat 4: from beat 4, the grid's line before it.
    const auto rowHeight = [&context] (phazite::TrackID track)
    { return phazite::ui::getTrackHeight (phazite::getTrackRow (context, track).value_or (phazite::TrackRow())); };
    const juce::Point<int> inLanes (juce::roundToInt (playlist.getView().beatToX (beats (4.5))),
                                    rowHeight (tracks[0]) + rowHeight (tracks[1]) / 2);
    const auto at = playlist.getLocalPoint (lanes, inLanes);
    dropTarget.fileDragMove ({ sound.getFullPathName() }, at.x, at.y);
    dropTarget.filesDropped ({ sound.getFullPathName() }, at.x, at.y);
    CHECK (phazite::getClips (context, tracks[1]).empty()); // read in the background (known bug 123): added once it is
    phazite::test::waitForAudioWork (context);
    phazite::test::dispatchPendingMessages();

    CHECK (phazite::getClips (context, tracks[0]).empty());
    const auto clips = phazite::getClips (context, tracks[1]);
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].range.getStart().inBeats() == doctest::Approx (4.0));
    REQUIRE (phazite::isAudioClip (song, clips[0].id));
    CHECK (phazite::getClipFile (song, clips[0].id).getFileNameWithoutExtension().startsWith ("Dropped"));
    CHECK (getSelection (context).isSelected (clips[0].id));
    REQUIRE (phazite::canUndo (song));
    phazite::undo (song);
    CHECK (phazite::getClips (context, tracks[1]).empty());
    CHECK_FALSE (phazite::canUndo (song));
}

TEST_CASE ("Only the tracks in view have headers, made as they scroll into view; one being renamed stays while it scrolls away and back")
{
    PlaylistScreen screen;
    const auto tracks = phazite::ensureTracks (screen.getSong(), 40);
    phazite::test::dispatchPendingMessages();

    const auto headerOf = [&screen] (phazite::TrackID track) -> phazite::ui::TrackHeader*
    {
        for (auto* header : phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist))
            if (header->getTrack() == track)
                return header;

        return nullptr;
    };

    // The rows in view and one more, not the last; a screen reader hears how many tracks there are.
    CHECK (phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist).size() < 20);
    REQUIRE (headerOf (tracks.front()) != nullptr);
    CHECK (headerOf (tracks.back()) == nullptr);
    auto* column = phazite::test::findComponentWithTitle (screen.playlist, "Track headers");
    REQUIRE (column != nullptr);
    CHECK (column->getDescription().startsWith ("40 tracks"));

    // The first track's name being typed.
    juce::Component::SafePointer<phazite::ui::TrackHeader> renamed (headerOf (tracks.front()));
    renamed->startRenaming();
    juce::Component::SafePointer<juce::TextEditor> field (phazite::test::findAll<juce::TextEditor> (*renamed).getFirst());
    REQUIRE (field != nullptr);
    REQUIRE (field->hasKeyboardFocus (false));
    field->setText ("Lead");

    // The rows scrolled to the last track and back meanwhile, as the playlist scrolls a lane just shown into view (JUCE's
    // label holds the mouse off while its name is typed, so the wheel can't): the header stays, and its field with what was
    // typed in it.
    juce::ScrollBar* rowsBar = nullptr;

    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.playlist))
        if (bar->isVertical() && bar->getParentComponent() == &screen.playlist)
            rowsBar = bar;

    REQUIRE (rowsBar != nullptr);
    rowsBar->setCurrentRangeStart (rowsBar->getMaximumRangeLimit(), juce::sendNotificationSync);
    phazite::test::dispatchPendingMessages(); // what a jump leaves is made a turn of the message loop later
    REQUIRE (headerOf (tracks.back()) != nullptr);
    CHECK (renamed != nullptr);
    CHECK (field != nullptr);
    CHECK (headerOf (tracks[1]) == nullptr); // the others near the top left

    rowsBar->setCurrentRangeStart (0.0, juce::sendNotificationSync);
    phazite::test::dispatchPendingMessages();
    REQUIRE (renamed != nullptr);
    CHECK (headerOf (tracks.front()) == renamed.getComponent());
    CHECK (headerOf (tracks.back()) == nullptr);
    REQUIRE (field != nullptr);
    CHECK (field->getText() == "Lead");

    // Enter keeps the name.
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getTrackRow (screen.context, tracks.front())->name == "Lead");
    REQUIRE (headerOf (tracks.front()) != nullptr);
    CHECK (headerOf (tracks.front())->getTitle() == "Lead");
}

TEST_CASE ("The Tab key goes on from the last track header in view to the next track's, which scrolls into view")
{
    PlaylistScreen screen;
    const auto tracks = phazite::ensureTracks (screen.getSong(), 40);
    phazite::test::dispatchPendingMessages();

    const auto headerOf = [&screen] (phazite::TrackID track) -> phazite::ui::TrackHeader*
    {
        for (auto* header : phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist))
            if (header->getTrack() == track)
                return header;

        return nullptr;
    };

    // The last header whole in view, its mute switch given the keys.
    const auto inView = [&screen] (juce::Component& header)
    {
        return screen.lanesArea().getY() <= header.getScreenY() - screen.playlist.getScreenY()
               && header.getScreenBounds().getBottom() <= screen.playlist.getScreenY() + screen.lanesArea().getBottom();
    };
    auto lastWhole = 0;

    while (headerOf (tracks[(size_t) lastWhole + 1]) != nullptr && inView (*headerOf (tracks[(size_t) lastWhole + 1])))
        ++lastWhole;

    auto* switches = phazite::test::findAll<phazite::ui::TrackSwitch> (*headerOf (tracks[(size_t) lastWhole])).getFirst();
    REQUIRE (switches != nullptr);
    switches->grabKeyboardFocus();
    REQUIRE (switches->hasKeyboardFocus (false));

    const auto next = tracks[(size_t) lastWhole + 1];
    const auto nextHasFocus = [&] { return headerOf (next) != nullptr && headerOf (next)->hasKeyboardFocus (true); };

    for (int i = 0; i < 20 && ! nextHasFocus(); ++i)
        screen.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));

    REQUIRE (nextHasFocus());
    phazite::test::dispatchPendingMessages(); // it scrolls into view as the focus has moved
    CHECK (inView (*headerOf (next)));
    CHECK (headerOf (tracks[(size_t) lastWhole + 2]) != nullptr); // the one after it, for the Tab key to go on to
}

TEST_CASE ("A clip dragged while a note is added to another goes where it is let go, and the other shows its note")
{
    PlaylistScreen screen;
    const auto dragged = screen.addClip (0, 0.0, 4.0);
    const auto other = screen.addClip (2, 8.0, 4.0);
    phazite::test::dispatchPendingMessages();

    const auto componentOf = [&screen] (phazite::ClipID clip) -> phazite::ui::ClipComponent*
    {
        for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
            if (component->getClip() == clip)
                return component;

        return nullptr;
    };

    juce::Component::SafePointer<phazite::ui::ClipComponent> draggedComponent (componentOf (dragged));
    REQUIRE (draggedComponent != nullptr);
    REQUIRE (componentOf (other) != nullptr);

    // The lower half of a clip moves it (the Smart Tool).
    phazite::test::Mouse mouse (*screen.lanes);
    const auto beat = screen.playlist.getView().pixelsPerBeat;
    const auto start = screen.at (1.0, 0).translated (0, phazite::ui::TrackHeight::standard / 4);
    mouse.press (start);
    mouse.dragTo (start.translated (juce::roundToInt (beat * 2.0), 0));
    const auto moving = screen.startOf (dragged);
    CHECK (moving > 0.0);

    // Meanwhile a note is added to the other clip (as the piano roll adds one, or Undo puts one back): that clip shows it, and
    // the dragged one stays where the drag has it, with its component.
    screen.window.takeRepaintedArea();
    phazite::addNote (screen.context, other, 60, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 100);
    phazite::test::dispatchPendingMessages();
    const auto repainted = screen.window.takeRepaintedArea().getBounds();
    CHECK (repainted.contains (screen.window.getLocalArea (screen.lanes, screen.boundsOf (other))));
    CHECK (draggedComponent != nullptr);
    CHECK (screen.boundsOf (dragged).getX() == screen.playlist.getView().beatToColumn (beats (moving)));

    mouse.dragTo (start.translated (juce::roundToInt (beat * 4.0), 0));
    mouse.release();
    phazite::test::dispatchPendingMessages();
    CHECK (screen.startOf (dragged) > moving);
    CHECK (draggedComponent != nullptr);
    CHECK (screen.boundsOf (dragged).getX() == screen.playlist.getView().beatToColumn (beats (screen.startOf (dragged))));
    CHECK (phazite::getClipNotes (screen.getSong(), other).size() == 1);
    CHECK (screen.startOf (other) == doctest::Approx (8.0));
}

TEST_CASE ("A track header dragged past the playlist's foot stays while the tracks scroll under it, and moves its track there")
{
    PlaylistScreen screen;
    const auto tracks = phazite::ensureTracks (screen.getSong(), 40);
    phazite::test::dispatchPendingMessages();

    const auto headerOf = [&screen] (phazite::TrackID track) -> phazite::ui::TrackHeader*
    {
        for (auto* header : phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist))
            if (header->getTrack() == track)
                return header;

        return nullptr;
    };

    // Pressed on its name, and dragged down past the lanes' foot, where the tracks scroll along.
    juce::Component::SafePointer<phazite::ui::TrackHeader> dragged (headerOf (tracks.front()));
    REQUIRE (dragged != nullptr);
    phazite::test::Mouse mouse (screen.playlist);
    const auto start = screen.playlist.getLocalPoint (dragged, juce::Point<int> (60, dragged->getHeight() / 2));
    mouse.press (start);

    for (int i = 0; i < 400 && headerOf (tracks.back()) == nullptr; ++i)
        mouse.dragTo ({ start.x, screen.playlist.getHeight() + 20 + i % 2 });

    REQUIRE (headerOf (tracks.back()) != nullptr); // scrolled to the end
    CHECK (dragged != nullptr);                    // and its header still there, out of view, with the mouse
    CHECK (headerOf (tracks[1]) == nullptr);

    // Let go under the last track: the track goes to the end.
    mouse.dragTo ({ start.x, screen.lanesArea().getBottom() - 4 });
    mouse.release();
    phazite::test::dispatchPendingMessages();
    const auto order = phazite::getAllPlaylistTracks (screen.context);
    REQUIRE (order.size() == tracks.size());
    CHECK (order.front() == tracks[1]);
    CHECK (std::find (order.end() - 3, order.end(), tracks.front()) != order.end());
}

namespace
{
// What a component draws, into a software image, children and all.
juce::Image pictureOf (juce::Component& component)
{
    juce::Image image (juce::Image::ARGB, std::max (1, component.getWidth()), std::max (1, component.getHeight()), true,
                       juce::SoftwareImageType());
    juce::Graphics g (image);
    component.paintEntireComponent (g, false);
    return image;
}

bool isSamePicture (const juce::Image& a, const juce::Image& b)
{
    if (a.getBounds() != b.getBounds())
        return false;

    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
            if (a.getPixelAt (x, y) != b.getPixelAt (x, y))
                return false;

    return true;
}

juce::ScrollBar* getRowsScrollBar (juce::Component& playlist)
{
    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (playlist))
        if (bar->isVertical() && bar->getParentComponent() == &playlist)
            return bar;

    return nullptr;
}

// What the playlist shows, as a second one made now on the same project shows it, which reads the whole song as it opens: the
// first brought only what changed up to date (Playlist::showChangedClips()). Every clip the same (where it is, its look, what
// it draws, what a screen reader hears of it), and the lanes (the folders' rows) and the overview the same pixels.
void checkShowsAsBuiltAfresh (PlaylistScreen& screen)
{
    phazite::test::dispatchPendingMessages();
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Playlist fresh { commands, screen.context };
    phazite::test::TestWindow window { fresh, screen.window.getWidth(), screen.window.getHeight() };
    phazite::test::dispatchPendingMessages();

    // The same rows in view.
    auto* shownBar = getRowsScrollBar (screen.playlist);
    auto* freshBar = getRowsScrollBar (fresh);
    REQUIRE (shownBar != nullptr);
    REQUIRE (freshBar != nullptr);
    freshBar->setCurrentRangeStart (shownBar->getCurrentRangeStart(), juce::sendNotificationSync);
    REQUIRE (juce::exactlyEqual (fresh.getView().pixelsPerBeat, screen.playlist.getView().pixelsPerBeat));
    REQUIRE (fresh.getView().start == screen.playlist.getView().start);

    auto* freshLanes = phazite::test::findComponentWithTitle (fresh, "Tracks");
    REQUIRE (freshLanes != nullptr);
    const auto shown = phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist);
    const auto made = phazite::test::findAll<phazite::ui::ClipComponent> (fresh);
    CHECK (shown.size() == made.size());

    for (auto* clip : made)
    {
        INFO ("the clip " << (juce::int64) clip->getClip().getRaw());
        const auto found = std::find_if (shown.begin(), shown.end(), [clip] (auto* c) { return c->getClip() == clip->getClip(); });
        REQUIRE (found != shown.end());
        auto* other = *found;
        CHECK (screen.lanes->getLocalArea (other, other->getLocalBounds()) == freshLanes->getLocalArea (clip, clip->getLocalBounds()));
        CHECK (other->getColour() == clip->getColour());
        CHECK (other->isFaint() == clip->isFaint());
        CHECK (other->isFrozen() == clip->isFrozen());
        CHECK (other->getLabel() == clip->getLabel());
        CHECK (other->getTitle() == clip->getTitle());
        CHECK (other->getDescription() == clip->getDescription());
        CHECK (isSamePicture (pictureOf (*other), pictureOf (*clip)));
    }

    CHECK (isSamePicture (pictureOf (*screen.lanes), pictureOf (*freshLanes)));
    auto* shownOverview = phazite::test::findComponentWithTitle (screen.playlist, "Overview");
    auto* freshOverview = phazite::test::findComponentWithTitle (fresh, "Overview");
    REQUIRE (shownOverview != nullptr);
    REQUIRE (freshOverview != nullptr);
    CHECK (isSamePicture (pictureOf (*shownOverview), pictureOf (*freshOverview)));
}

phazite::ui::ClipComponent* findClipComponent (PlaylistScreen& screen, phazite::ClipID clip)
{
    for (auto* component : phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist))
        if (component->getClip() == clip)
            return component;

    return nullptr;
}
} // namespace

TEST_CASE ("A clip dragged to another track takes that track's look, as a playlist made afresh shows it, and back with Undo and Redo")
{
    PlaylistScreen screen;
    const auto red = juce::Colour (0xffd04040), blue = juce::Colour (0xff4060d0);
    phazite::setTrackColours (screen.context, { screen.tracks[0] }, red);
    phazite::setTrackColours (screen.context, { screen.tracks[1] }, blue);
    phazite::toggleMute (screen.context, screen.tracks[2]); // not heard
    const auto dragged = screen.addClip (0, 0.0, 4.0);
    const auto unheard = screen.addClip (2, 8.0, 4.0);
    phazite::test::dispatchPendingMessages();
    REQUIRE (findClipComponent (screen, dragged) != nullptr);
    CHECK (findClipComponent (screen, dragged)->getColour() == red);
    CHECK (findClipComponent (screen, unheard)->isFaint());

    // Dragged by its lower half (the Smart Tool moves it) down to the blue track: blue, in the middle of the drag and after.
    phazite::test::Mouse mouse (*screen.lanes);
    const auto start = screen.at (1.0, 0).translated (0, phazite::ui::TrackHeight::standard / 4);
    mouse.press (start);
    mouse.dragTo (start.translated (4, screen.heightOf (0)));
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.getSong().getUndoManager().canUndo());
    CHECK (phazite::getClipTrack (screen.getSong(), dragged) == screen.tracks[1]);
    CHECK (findClipComponent (screen, dragged)->getColour() == blue);
    checkShowsAsBuiltAfresh (screen);
    mouse.release();
    checkShowsAsBuiltAfresh (screen);

    // From the track that isn't heard to one that is: no longer faint, and a screen reader no longer hears that it isn't.
    const auto from = screen.at (9.0, 2).translated (0, phazite::ui::TrackHeight::standard / 4);
    mouse.press (from);
    mouse.dragTo (from.translated (4, -screen.heightOf (1)));
    mouse.release();
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::getClipTrack (screen.getSong(), unheard) == screen.tracks[1]);
    CHECK_FALSE (findClipComponent (screen, unheard)->isFaint());
    CHECK_FALSE (findClipComponent (screen, unheard)->getDescription().contains ("isn't heard"));
    checkShowsAsBuiltAfresh (screen);

    // Undo puts each back on its track, in its look, and Redo again.
    for (int i = 0; i < 2; ++i)
    {
        phazite::undo (screen.getSong());
        checkShowsAsBuiltAfresh (screen);
    }

    CHECK (findClipComponent (screen, dragged)->getColour() == red);
    CHECK (findClipComponent (screen, unheard)->isFaint());

    for (int i = 0; i < 2; ++i)
    {
        phazite::redo (screen.getSong());
        checkShowsAsBuiltAfresh (screen);
    }

    CHECK (findClipComponent (screen, dragged)->getColour() == blue);
    CHECK_FALSE (findClipComponent (screen, unheard)->isFaint());
}

TEST_CASE ("A note added to a clip with aliases on other tracks shows on every one of them, as a playlist made afresh shows it")
{
    PlaylistScreen screen;
    const auto clip = screen.addClip (0, 0.0, 4.0);
    phazite::test::dispatchPendingMessages();
    const auto aliases = phazite::copyClips (screen.context, { clip }, true);
    REQUIRE (aliases.size() == 1);
    phazite::moveClip (screen.context, aliases[0], beats (8.0), screen.tracks[2]);
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::getLinkedClips (screen.context, clip).size() == 2);

    screen.window.takeRepaintedArea();
    phazite::addNote (screen.context, clip, 64, beats (1.0), phazite::BeatDuration::fromBeats (1.0), 100);
    phazite::test::dispatchPendingMessages();
    const auto repainted = screen.window.takeRepaintedArea();

    for (const auto each : { clip, aliases[0] })
    {
        INFO ("the clip " << (juce::int64) each.getRaw());
        CHECK (phazite::getClipNotes (screen.getSong(), each).size() == 1);
        CHECK (repainted.containsRectangle (screen.window.getLocalArea (screen.lanes, screen.boundsOf (each))));
    }

    checkShowsAsBuiltAfresh (screen);
}

TEST_CASE ("A clip moved into a collapsed folder's track and out again shows in the folder's row and the overview, as made afresh")
{
    PlaylistScreen screen;
    const auto folder = phazite::addTrackAtEnd (screen.context, true);
    phazite::moveTracksTo (screen.context, { screen.tracks[2] }, folder, phazite::TrackDrop::into);
    phazite::setCollapsed (screen.context, folder, true);
    const auto clip = screen.addClip (0, 0.0, 4.0);
    phazite::test::dispatchPendingMessages();
    checkShowsAsBuiltAfresh (screen);

    // Into the hidden track: off the first row, onto the folder's.
    phazite::moveClip (screen.context, clip, beats (4.0), screen.tracks[2]);
    phazite::test::dispatchPendingMessages();
    CHECK (findClipComponent (screen, clip) == nullptr);
    checkShowsAsBuiltAfresh (screen);

    // A note added to it there, and moved within it: the folder's row and the overview follow.
    phazite::addNote (screen.context, clip, 60, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 100);
    checkShowsAsBuiltAfresh (screen);
    phazite::moveClip (screen.context, clip, beats (12.0), screen.tracks[2]);
    checkShowsAsBuiltAfresh (screen);

    // And out again.
    phazite::moveClip (screen.context, clip, beats (2.0), screen.tracks[1]);
    phazite::test::dispatchPendingMessages();
    CHECK (findClipComponent (screen, clip) != nullptr);
    checkShowsAsBuiltAfresh (screen);
}

TEST_CASE ("Shift+Tab goes back from the first track header in view to the one above it, which scrolls into view")
{
    PlaylistScreen screen;
    const auto tracks = phazite::ensureTracks (screen.getSong(), 40);
    phazite::test::dispatchPendingMessages();

    const auto headerOf = [&screen] (phazite::TrackID track) -> phazite::ui::TrackHeader*
    {
        for (auto* header : phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist))
            if (header->getTrack() == track)
                return header;

        return nullptr;
    };

    const auto inView = [&screen] (juce::Component& header)
    {
        const auto area = screen.lanesArea() + screen.playlist.getScreenPosition();
        return area.getY() <= header.getScreenY() && header.getScreenBounds().getBottom() <= area.getBottom();
    };

    // Scrolled half way down, a part of a row more, so that the row above the first whole one is cut by the view's top.
    auto* bar = getRowsScrollBar (screen.playlist);
    REQUIRE (bar != nullptr);
    bar->setCurrentRangeStart (bar->getMaximumRangeLimit() / 2 + 7, juce::sendNotificationSync);
    phazite::test::dispatchPendingMessages();
    auto firstWhole = 0;

    while (headerOf (tracks[(size_t) firstWhole]) == nullptr || ! inView (*headerOf (tracks[(size_t) firstWhole])))
        ++firstWhole;

    REQUIRE (firstWhole >= 2);
    auto* switches = phazite::test::findAll<phazite::ui::TrackSwitch> (*headerOf (tracks[(size_t) firstWhole])).getFirst();
    REQUIRE (switches != nullptr);
    switches->grabKeyboardFocus();

    const auto previous = tracks[(size_t) firstWhole - 1];
    const auto previousHasFocus = [&] { return headerOf (previous) != nullptr && headerOf (previous)->hasKeyboardFocus (true); };

    for (int i = 0; i < 20 && ! previousHasFocus(); ++i)
        screen.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0));

    REQUIRE (previousHasFocus());
    phazite::test::dispatchPendingMessages(); // it scrolls into view as the focus has moved
    CHECK (inView (*headerOf (previous)));
    CHECK (headerOf (tracks[(size_t) firstWhole - 2]) != nullptr); // the one before it, for the keys to go on to
}

TEST_CASE ("Only the automation lanes in view have components; a point dragged in one stays dragged while the view scrolls")
{
    PlaylistScreen screen;
    const auto tracks = phazite::ensureTracks (screen.getSong(), 40);

    for (const auto track : tracks)
    {
        const auto volume = phazite::getVolume (screen.context, track);
        phazite::showAutomationLane (volume);
        phazite::addAutomationPoint (screen.context, volume.getRef(), { beats (2.0), 0.5f });
    }

    phazite::test::dispatchPendingMessages();
    const auto volume = phazite::getVolume (screen.context, tracks.front()).getRef();

    // The first track's lane is under its row: its point, in the middle of the lane's height at beat 2.
    const auto row = screen.heightOf (0);
    auto* laneView = screen.lanes->getComponentAt (screen.at (2.0, 0).withY (row + 4));
    REQUIRE (laneView != nullptr);
    REQUIRE (laneView != screen.lanes);
    juce::Component::SafePointer<juce::Component> pressedLane (laneView);
    const auto lanePoint =
        juce::Point<int> (screen.at (2.0, 0).x, screen.lanes->getLocalArea (laneView, laneView->getLocalBounds()).getCentreY());

    // The lanes far down have no components.
    auto* bar = getRowsScrollBar (screen.playlist);
    REQUIRE (bar != nullptr);
    const auto countAll = [&screen] { return phazite::test::findAll<juce::Component> (screen.playlist).size(); };
    const auto before = countAll();

    // The point pressed and dragged up; the view scrolled to the end meanwhile, and back.
    phazite::test::Mouse mouse (*screen.lanes);
    mouse.press (lanePoint);
    mouse.dragTo (lanePoint.translated (0, -6));
    bar->setCurrentRangeStart (bar->getMaximumRangeLimit(), juce::sendNotificationSync);
    CHECK (pressedLane != nullptr); // kept, while its point is dragged
    CHECK (countAll() < before + 10);
    bar->setCurrentRangeStart (0.0, juce::sendNotificationSync);
    REQUIRE (pressedLane != nullptr);
    mouse.dragTo (lanePoint.translated (0, -12));
    mouse.release();
    phazite::test::dispatchPendingMessages();
    const auto points = phazite::getAutomationPoints (screen.context, volume);
    REQUIRE (points.size() == 1);
    CHECK (points[0].position > 0.5f);

    // One undo step, which puts it back.
    phazite::undo (screen.getSong());
    CHECK (phazite::getAutomationPoints (screen.context, volume)[0].position == doctest::Approx (0.5f));
}

//==============================================================================
// Scrolls by hand while the display's frames come (TimelineEditor::scrollViewTo()). The owner, 2026-11-16: "when scrolling
// the horizontal bar on a playlist song, it can be visually choppy/laggy", the song playing, either bar dragged, in the
// release build: a mouse that reports a thousand times a second moved the view at each report.
namespace
{
// Where the view is in the song's pixels, the part of a step its parts are shifted by counted in.
double placeOf (const phazite::ui::Playlist& playlist)
{
    return playlist.getView().start.inBeats() * playlist.getView().pixelsPerBeat + (double) playlist.getSubPixel();
}

juce::ScrollBar* getTimelineScrollBar (phazite::ui::Playlist& playlist)
{
    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (playlist))
        if (! bar->isVertical())
            return bar;

    return nullptr;
}
} // namespace

TEST_CASE ("While the display's frames come, the overview's drag moves the view once a frame, gliding to where the pointer took it")
{
    PlaylistScreen screen;
    REQUIRE (screen.addClip (0, 0.0, 4.0).isValid());
    REQUIRE (screen.addClip (2, 32.0, 4.0).isValid());
    phazite::test::dispatchPendingMessages();
    auto* overview = phazite::test::findComponentWithTitle (screen.playlist, "Overview");
    auto* timeline = getTimelineScrollBar (screen.playlist);
    REQUIRE (overview != nullptr);
    REQUIRE (timeline != nullptr);
    auto& playlist = screen.playlist;
    CHECK_FALSE (playlist.isScrollingAtFrames()); // a window on no screen: scrolls go at once, as every other test has them
    playlist.setScrollingAtFrames (true);

    // Sixteen of the mouse's reports, a pixel of the overview each, as a frame's are at a thousand a second: none moves the
    // view, and the thumb, which shows where the view is going, goes with the pointer.
    const auto before = playlist.getView().start;
    const auto shown = timeline->getCurrentRange();
    const juce::Point<int> thumb (juce::roundToInt (shown.getLength() / timeline->getRangeLimit().getLength() * overview->getWidth() / 2.0),
                                  overview->getHeight() / 2);
    phazite::test::Mouse mouse (*overview);
    mouse.setEventInterval (1);
    mouse.press (thumb);

    for (int report = 1; report <= 16; ++report)
        mouse.dragTo (thumb.translated (report, 0));

    CHECK (playlist.getView().start == before);
    const auto target = playlist.getScrollTarget();
    CHECK (target > before);
    CHECK (timeline->getCurrentRange().getStart() == doctest::Approx (target.inBeats()));
    const auto targetPlace = target.inBeats() * playlist.getView().pixelsPerBeat;
    REQUIRE (targetPlace - placeOf (playlist) > 20.0); // a pixel of the overview is several of the song

    // The frames: each takes the view a share of the way, never back, between pixels (the view on whole steps, its parts
    // shifted by the rest), until it rests on the target's whole step.
    auto seconds = 100.0, last = placeOf (playlist);
    auto frames = 0, between = 0;

    while (playlist.getScrollTarget() != playlist.getView().start || playlist.getSubPixel() > 0.0f)
    {
        REQUIRE (++frames < 60);
        seconds += 1.0 / 60.0;
        playlist.showFrame (seconds);
        const auto now = placeOf (playlist);
        CHECK (now > last);
        CHECK (now <= targetPlace);
        CHECK (playlist.getSubPixel() >= 0.0f);
        CHECK (playlist.getSubPixel() < (float) playlist.getViewStep());
        CHECK (playlist.getView().start.inBeats() * playlist.getView().pixelsPerBeat
               == doctest::Approx (std::round (playlist.getView().start.inBeats() * playlist.getView().pixelsPerBeat)));
        between += playlist.getSubPixel() > 0.0f ? 1 : 0;
        last = now;
    }

    CHECK (frames >= 4);  // it glides
    CHECK (frames <= 20); // and is there within a third of a second
    CHECK (between >= 2);
    CHECK (playlist.getView().start == target);

    // A frame more moves nothing, and the mouse let go leaves the view there.
    playlist.showFrame (seconds + 1.0 / 60.0);
    mouse.release();
    CHECK (playlist.getView().start == target);
    CHECK (playlist.getSubPixel() == 0.0f);

    // The wheel, two notches before a frame: both counted, from where the view was going.
    const auto centre = screen.lanesArea().getCentre();
    phazite::test::Mouse wheel (playlist);
    wheel.wheel (centre, -0.1f, juce::ModifierKeys::shiftModifier);
    const auto afterOne = playlist.getScrollTarget();
    CHECK (afterOne > target);
    CHECK (playlist.getView().start == target);
    wheel.wheel (centre, -0.1f, juce::ModifierKeys::shiftModifier);

    // As far again, to a step of the view: each lands on a whole one, and a notch isn't a whole count of them everywhere
    // (Linux's is another than Windows').
    const auto stepInBeats = playlist.getViewStep() / playlist.getView().pixelsPerBeat;
    CHECK (std::abs ((playlist.getScrollTarget() - afterOne).inBeats() - (afterOne - target).inBeats()) <= stepInBeats + 1.0e-9);

    // The view put somewhere meanwhile (a zoom, a command) drops what was on its way.
    playlist.setViewStart (before);
    CHECK (playlist.getScrollTarget() == before);
    playlist.showFrame (seconds + 2.0 / 60.0);
    CHECK (playlist.getView().start == before);

    // No longer waiting for frames, what waits goes at once.
    wheel.wheel (centre, -0.1f, juce::ModifierKeys::shiftModifier);
    CHECK (playlist.getView().start == before);
    playlist.setScrollingAtFrames (false);
    const auto scrolled = playlist.getView().start;
    CHECK (scrolled > before);

    // And a pan, the song held by the middle button, goes straight where the pointer took it at the next frame.
    playlist.setScrollingAtFrames (true);
    phazite::test::Mouse pan (*screen.lanes);
    pan.press ({ 300, 20 }, juce::ModifierKeys::middleButtonModifier);
    pan.dragTo ({ 280, 20 });
    pan.dragTo ({ 270, 20 });
    CHECK (playlist.getView().start == scrolled);
    playlist.showFrame (seconds + 3.0 / 60.0);
    CHECK ((playlist.getView().start - scrolled).inBeats() * playlist.getView().pixelsPerBeat == doctest::Approx (30.0));
    CHECK (playlist.getSubPixel() == 0.0f);
    pan.release();
}

TEST_CASE ("While the display's frames come, the rows' scroll bar and the wheel scroll the rows once a frame")
{
    PlaylistScreen screen;
    phazite::ensureTracks (screen.getSong(), 30);
    phazite::test::dispatchPendingMessages();
    auto& playlist = screen.playlist;
    auto* bar = getRowsScrollBar (playlist);
    REQUIRE (bar != nullptr);
    REQUIRE (bar->isVisible());

    // How far the rows are scrolled: the track headers' holder, moved up by it.
    auto* header = phazite::test::findAll<phazite::ui::TrackHeader> (playlist).getFirst();
    REQUIRE (header != nullptr);
    auto* headers = header->getParentComponent();
    const auto scrolled = [headers] { return -headers->getY(); };
    REQUIRE (scrolled() == 0);
    playlist.setScrollingAtFrames (true);

    // The bar moved three times before a frame, as the mouse's reports move it: the rows go where the last put them, once.
    for (const auto y : { 40.0, 90.0, 120.0 })
        bar->setCurrentRangeStart (y, juce::sendNotificationSync);

    CHECK (scrolled() == 0);
    playlist.showFrame (100.0);
    CHECK (scrolled() == 120);
    CHECK (bar->getCurrentRangeStart() == doctest::Approx (120.0));

    // The wheel, two notches before a frame: both counted.
    phazite::test::Mouse mouse (playlist);
    const auto centre = screen.lanesArea().getCentre();
    mouse.wheel (centre, -0.1f);
    mouse.wheel (centre, -0.1f);
    CHECK (scrolled() == 120);
    playlist.showFrame (100.0 + 1.0 / 60.0);
    const auto afterWheel = scrolled();
    CHECK (afterWheel > 120);

    // Without frames, a notch at once: half of the two, to the part of a pixel the wheel keeps from each turn for the next.
    playlist.setScrollingAtFrames (false);
    mouse.wheel (centre, -0.1f);
    CHECK (std::abs ((scrolled() - afterWheel) * 2 - (afterWheel - 120)) <= 2);

    // The mouse's pan of the rows, likewise once a frame.
    playlist.setScrollingAtFrames (true);
    const auto beforePan = scrolled();
    phazite::test::Mouse pan (*screen.lanes);
    pan.press ({ 300, 200 }, juce::ModifierKeys::middleButtonModifier);
    pan.dragTo ({ 300, 180 });
    pan.dragTo ({ 300, 170 });
    CHECK (scrolled() == beforePan);
    playlist.showFrame (100.0 + 2.0 / 60.0);
    CHECK (scrolled() == beforePan + 30);

    // Escape takes the pan back, what its last move asked for with it.
    pan.dragTo ({ 300, 100 });
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    playlist.showFrame (100.0 + 3.0 / 60.0);
    CHECK (scrolled() == beforePan);
    pan.release();
}

TEST_CASE ("Scrolls by hand stop waiting for the display's frames once they stop coming")
{
    PlaylistScreen screen;
    auto& playlist = screen.playlist;

    // A frame of the display, as its vertical blank gives one: scrolls wait for the next from then on.
    playlist.showDisplayFrame (100.0);
    CHECK (playlist.isScrollingAtFrames());
    const auto before = playlist.getView().start;
    phazite::test::Mouse mouse (playlist);
    mouse.wheel (screen.lanesArea().getCentre(), -0.1f, juce::ModifierKeys::shiftModifier);
    const auto target = playlist.getScrollTarget();
    REQUIRE (target > before);

    // No more come (a display asleep): by the editor's second tick the view is where the wheel took it, and scrolls go at once.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&playlist] { return ! playlist.isScrollingAtFrames(); }));
    CHECK (playlist.getView().start == target);
    mouse.wheel (screen.lanesArea().getCentre(), -0.1f, juce::ModifierKeys::shiftModifier);
    CHECK (playlist.getView().start > target);
}

TEST_CASE ("The playlist asks for the song's whole extent only once the song has changed, at the next tick of its timer")
{
    // The threading audit, 2026-11-20: each editor that showed asked for where its content ends five times a second, which
    // walks the whole song (its clips, sections and chords), while nothing changed.
    PlaylistScreen screen;
    auto& playlist = screen.playlist;
    REQUIRE (playlist.isShowing());
    playlist.tickNow(); // what changed as it was made
    const auto asks = playlist.getContentAsks();

    for (int i = 0; i < 20; ++i)
        playlist.tickNow();

    CHECK (playlist.getContentAsks() == asks);

    // A clip far past the song's end: the next tick asks once, and the scroll bar reaches past it.
    const auto scrollsTo = [&playlist]
    {
        auto most = 0.0;

        for (auto* bar : phazite::test::findAll<juce::ScrollBar> (playlist))
            if (! bar->isVertical())
                most = std::max (most, bar->getMaximumRangeLimit());

        return most;
    };

    REQUIRE (scrollsTo() < 1000.0);
    screen.addClip (0, 1000.0, 4.0);
    playlist.tickNow();
    CHECK (playlist.getContentAsks() == asks + 1);
    CHECK (scrollsTo() > 1004.0);

    for (int i = 0; i < 20; ++i)
        playlist.tickNow();

    CHECK (playlist.getContentAsks() == asks + 1);
}
