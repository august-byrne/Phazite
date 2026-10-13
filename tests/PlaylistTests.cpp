// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist, driven with the mouse and the keys as the user would.
#include "UiTestHelpers.h"
#include "model/Clips.h"
#include "model/Outlines.h"
#include "model/Selection.h"
#include "model/Tracks.h"
#include "ui/Playlist.h"
#include "ui/TrackHeader.h"

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
        selection.edit = edit.get();

        phazite::ui::addTimelineCommands (registry, [this] { return &playlist; });
        playlist.addCommands (registry);
        playlist.setEdit (edit.get());
        window.addKeyListener (&registry); // as the main window does
        phazite::test::dispatchPendingMessages();

        lanes = phazite::test::findComponentWithTitle (playlist, "Tracks");
        REQUIRE (lanes != nullptr);
    }

    ~PlaylistScreen()
    {
        window.removeKeyListener (&registry);
        playlist.setEdit (nullptr);
        selection.deselectAll();
    }

    // Where a beat is on a track's row, in the lanes (the middle of the row).
    juce::Point<int> at (double beat, int track) const
    {
        auto y = 0;

        for (int i = 0; i < track; ++i)
            y += phazite::getTrackHeight (*tracks[i]);

        return { juce::roundToInt (playlist.getView().beatToX (beats (beat))), y + phazite::getTrackHeight (*tracks[track]) / 2 };
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
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::RangeSelection range;
    phazite::CommandRegistry registry; // with the default keymap
    phazite::ui::Playlist playlist { registry, selection, range };
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
    CHECK (screen.playlist.getTool() == phazite::Tool::paint);

    // A hold while clicking goes back to the tool before when the key is let go.
    REQUIRE (screen.registry.invoke ("tool.smart"));
    screen.window.pressKey (keyB);
    mouse.click (screen.at (20.0, 2));
    CHECK (screen.playlist.getTool() == phazite::Tool::paint);
    screen.window.releaseKeys();
    CHECK (screen.playlist.getTool() == phazite::Tool::smart);

    // Let go during a drag: the drag ends with the tool it began with.
    screen.window.pressKey (juce::KeyPress ('8', 0, '8')); // the Select tool, by its digit
    mouse.press (screen.at (20.0, 2));
    mouse.dragTo (screen.at (24.0, 1));
    screen.window.releaseKeys();
    CHECK (screen.playlist.getTool() == phazite::Tool::select);
    mouse.release();
    CHECK (screen.playlist.getTool() == phazite::Tool::smart);
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
    const auto start = screen.at (1.0, 0).translated (0, phazite::TrackHeight::standard / 4);
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
    mouse.doubleClick ({ 100, 4 * phazite::TrackHeight::standard + 40 });
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
    REQUIRE (header.getTrack() == screen.tracks[0]);

    phazite::test::Mouse mouse (screen.playlist);
    const auto inPlaylist = [&screen, &header] (juce::Point<int> p) { return screen.playlist.getLocalPoint (&header, p); };
    const auto down = juce::Point<int> (0, 2 * phazite::TrackHeight::standard + 40); // near the boundary under the third track

    // Dragging the middle of the header moves nothing.
    const auto middle = inPlaylist (header.getLocalBounds().getCentre());
    mouse.press (middle);
    mouse.dragTo (middle + down);
    mouse.release();
    phazite::test::dispatchPendingMessages();
    CHECK (te::getAudioTracks (*screen.edit) == screen.tracks);

    // Dragging the handle does: the track goes last.
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
    CHECK (header.getHeight() == phazite::TrackHeight::collapsed);
    CHECK (phazite::test::findComponentWithTitle (header, "Expand track") != nullptr);

    // Dragging its bottom edge down expands it to where the edge is let go.
    const auto edge = inPlaylist ({ header.getWidth() / 2, header.getHeight() - 2 });
    mouse.press (edge);
    mouse.dragTo (edge + juce::Point<int> (0, 30));
    mouse.release();
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::isCollapsed (*screen.tracks[0]));
    CHECK (header.getHeight() == phazite::TrackHeight::collapsed + 30);
}

TEST_CASE ("With FL Studio's keymap, a click on empty space places the last clip used, and right-click deletes it")
{
    PlaylistScreen screen;
    screen.registry.setKeymap (*phazite::Keymap::findBuiltIn ("FL Studio"));
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
    screen.registry.setKeymap (*phazite::Keymap::findBuiltIn ("Phazite"));
    mouse.click (screen.at (8.05, 1));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getClipStarts (1).empty());
    CHECK (screen.selection.isSelected (screen.tracks[1]));
}
