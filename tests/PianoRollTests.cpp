// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll, driven with the mouse and the selection as the user would.
#include "GoldenFiles.h"
#include "TestDeviceValues.h"
#include "UiTestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

// A piano roll in a window, with two tracks and the commands the app gives it. It follows the selection.
struct PianoRollScreen
{
    PianoRollScreen()
    {
        tracks = phazite::ensureTracks (song, 2);
        song.getUndoManager().clearUndoHistory();

        phazite::ui::addTimelineCommands (registry, [this] { return &pianoRoll; });
        pianoRoll.addCommands (registry);
        setSong (context, &song);
        phazite::test::dispatchPendingMessages();

        notes = phazite::test::findComponentWithTitle (pianoRoll, "Notes");
        REQUIRE (notes != nullptr);
        REQUIRE (dynamic_cast<phazite::ui::TimelinePartWindow*> (notes->getParentComponent()) != nullptr);
    }

    // Where the notes show, in the piano roll: the window they show through, which they are a pixel wider than
    // (TimelineEditor::addTimelinePart()). At rest, their own x 0 is its left.
    juce::Rectangle<int> notesArea() const { return notes->getParentComponent()->getBounds(); }

    ~PianoRollScreen() { setSong (context, nullptr); }

    phazite::ClipID addClip (int track, double start, double length)
    {
        const auto clip = phazite::insertMidiClip (song, tracks[(size_t) track], { beats (start), beats (start + length) }, nullptr);
        REQUIRE (clip.isValid());
        return clip;
    }

    void select (phazite::ClipID clip)
    {
        selection.selectOnly (clip);
        phazite::test::dispatchPendingMessages();
    }

    void select (phazite::TrackID track)
    {
        selection.selectOnly (track);
        phazite::test::dispatchPendingMessages();
    }

    // Where a beat is in the note grid, halfway down.
    juce::Point<int> at (double beat) const
    {
        return { juce::roundToInt (pianoRoll.getView().beatToX (beats (beat))), notes->getHeight() / 2 };
    }

    // A clip's notes, in the order the song holds them.
    std::vector<phazite::NoteData> notesOf (phazite::ClipID clip) const { return phazite::getClipNotes (song, clip); }

    phazite::Song::Ptr songPtr = phazite::createSong(); // 120 BPM, 4/4
    phazite::Song& song = *songPtr;
    std::vector<phazite::TrackID> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    phazite::Selection& selection = getSelection (context);
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::PianoRoll pianoRoll { commands, context };
    // Its notes as large as they were before its settings went down its left and its tools into a tab row (2026-11-13), so
    // that the grid adapts to the same step.
    phazite::test::TestWindow window { pianoRoll, 1176, 564 };
    juce::Component* notes = nullptr;
};
} // namespace

TEST_CASE ("The piano roll keeps the notes being edited in view while the song plays, unless asked to follow it")
{
    PianoRollScreen screen;
    CHECK_FALSE (screen.pianoRoll.isFollowingPlayback());

    // View > Follow Playback turns it on for the editor used last, as the playlist has it from the start.
    REQUIRE (screen.registry.invoke ("view.followPlayback"));
    CHECK (screen.pianoRoll.isFollowingPlayback());
}

TEST_CASE ("The piano roll's overview of its notes runs along its time bar's top, and its rows' scroll bar lies thin over the notes")
{
    // As the playlist's (the owner, 2026-11-06): FL Studio's scroll bars, see-through, the horizontal one on top over a
    // miniature of what the editor shows.
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 0.0, 16.0);
    phazite::addNote (screen.song, clip, 48, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 100);
    phazite::addNote (screen.song, clip, 84, beats (8.0), phazite::BeatDuration::fromBeats (2.0), 100);
    screen.select (clip);

    auto* overview = phazite::test::findComponentWithTitle (screen.pianoRoll, "Overview");
    REQUIRE (overview != nullptr);
    CHECK (overview->isVisible());
    CHECK (overview->getBottom() <= screen.notesArea().getY());
    CHECK (overview->getX() == screen.notesArea().getX());

    // No scroll bar under the notes: the overview scrolls them sideways. The rows' bar, while they don't fit, over the notes'
    // right edge, thin and see-through.
    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.pianoRoll))
    {
        // The settings' own, down the left, which scrolls them where the piano roll is too short for them.
        if (bar->findParentComponentOfClass<juce::Viewport>() != nullptr)
            continue;

        if (! bar->isVertical())
        {
            CHECK_FALSE (bar->isVisible());
            continue;
        }

        CHECK (bar->getWidth() == phazite::ui::ScrollTint::overlayWidth);
        CHECK (bar->getRight() == screen.notesArea().getRight());
        CHECK_FALSE (bar->findColour (juce::ScrollBar::trackColourId).isTransparent());
    }

    // A press on the overview's right end brings the end of the time line into view.
    const auto before = screen.pianoRoll.getView().start.inBeats();
    phazite::test::Mouse mouse (*overview);
    mouse.click ({ overview->getWidth() - 3, overview->getHeight() / 2 });
    CHECK (screen.pianoRoll.getView().start.inBeats() > before);
}

TEST_CASE ("A note fills its row from the line above it to the line below it, as tall above its middle as below")
{
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 0.0, 4.0);
    phazite::addNote (screen.song, clip, 62, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 100); // D: no octave line
    screen.select (clip);
    const auto image = screen.notes->createComponentSnapshot (screen.notes->getLocalBounds());

    // The note's column and one as far into a later beat of the clip, where no note is: the note is where they differ.
    const auto noteX = screen.at (0.6).x, emptyX = screen.at (2.6).x;
    std::vector<int> covered;

    for (int y = 0; y < image.getHeight(); ++y)
        if (image.getPixelAt (noteX, y) != image.getPixelAt (emptyX, y))
            covered.push_back (y);

    REQUIRE_FALSE (covered.empty());
    const auto top = covered.front(), bottom = covered.back();
    CHECK (bottom - top + 1 == (int) covered.size()); // one piece

    // Its row in the empty column: the lines that part it from the rows above and below, the first pixels from the note's
    // middle that are another colour than the row.
    const auto middle = (top + bottom) / 2;
    const auto rowColour = image.getPixelAt (emptyX, middle);
    auto lineAbove = middle, lineBelow = middle;

    while (lineAbove > 0 && image.getPixelAt (emptyX, lineAbove) == rowColour)
        --lineAbove;

    while (lineBelow < image.getHeight() - 1 && image.getPixelAt (emptyX, lineBelow) == rowColour)
        ++lineBelow;

    CHECK (top == lineAbove + 1);
    CHECK (bottom == lineBelow - 1);
}

TEST_CASE ("The piano roll's keys are a piano's: white keys parted in the middle of the black keys between them")
{
    PianoRollScreen screen;
    screen.select (screen.addClip (0, 0.0, 4.0));

    auto* keys = phazite::test::findComponentWithTitle (screen.pianoRoll, "Keys");
    REQUIRE (keys != nullptr);
    const auto image = keys->createComponentSnapshot (keys->getLocalBounds());
    const auto& p = phazite::ui::getPalette (*keys);
    const auto isNear = [] (juce::Colour a, juce::Colour b)
    {
        return std::abs ((int) a.getRed() - (int) b.getRed()) <= 2 && std::abs ((int) a.getGreen() - (int) b.getGreen()) <= 2
               && std::abs ((int) a.getBlue() - (int) b.getBlue()) <= 2;
    };

    // The black keys, from the left of the keys; the lines between white keys, just right of the black keys (left of
    // where C's names show).
    const auto black = p.background.interpolatedWith (juce::Colours::black, 0.6f);
    std::vector<juce::Range<int>> blackKeys;

    for (int y = 0; y < image.getHeight(); ++y)
    {
        if (! isNear (image.getPixelAt (1, y), black))
            continue;

        if (! blackKeys.empty() && blackKeys.back().getEnd() == y)
            blackKeys.back().setEnd (y + 1);
        else
            blackKeys.push_back ({ y, y + 1 });
    }

    const auto linesIn = [&] (int from, int to)
    {
        auto lines = 0;

        for (auto y = from; y < to; ++y)
            lines += isNear (image.getPixelAt (keys->getWidth() * 2 / 3 + 2, y), p.border) ? 1 : 0;

        return lines;
    };

    REQUIRE (blackKeys.size() >= 10);
    const auto rowHeight = blackKeys[1].getLength();

    // Each black key whole in view: one line on its right, where the white keys beside it meet. Between two black keys,
    // one white key without a line across it (D, G, A), or two parted by one (E and F, B and C).
    for (size_t i = 1; i + 1 < blackKeys.size(); ++i)
    {
        CAPTURE (i);
        CHECK (blackKeys[i].getLength() == rowHeight);
        CHECK (linesIn (blackKeys[i].getStart(), blackKeys[i].getEnd()) == 1);

        if (i + 2 < blackKeys.size())
        {
            const auto gap = blackKeys[i + 1].getStart() - blackKeys[i].getEnd();
            CHECK (linesIn (blackKeys[i].getEnd(), blackKeys[i + 1].getStart()) == (gap > rowHeight * 3 / 2 ? 1 : 0));
        }
    }
}

TEST_CASE ("While a clip linked to the one shown plays, the piano roll's playhead shows where its notes are playing")
{
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 0.0, 4.0);
    const auto aliases = phazite::copyClips (screen.song, { clip }, true);
    REQUIRE (aliases.size() == 1);
    phazite::moveClip (screen.song, aliases[0], beats (8.0), phazite::getClipTrack (screen.song, aliases[0])); // beats 8 to 12
    screen.select (clip);

    auto* playhead = phazite::test::findComponentWithTitle (screen.pianoRoll, "Playhead");
    REQUIRE (playhead != nullptr);
    const auto playheadAt = [&screen, playhead] (double beat)
    {
        return std::abs (playhead->getBounds().getCentreX()
                         - (screen.notesArea().getX() + screen.pianoRoll.getView().beatToX (beats (beat))))
               <= 1.0;
    };

    // At beat 9.5 the alias plays beat 1.5 of the notes.
    phazite::movePlayhead (screen.song, beats (9.5));
    CHECK (phazite::test::dispatchMessagesUntil ([&screen] { return screen.pianoRoll.isPlayheadShowingLinkedClip(); }));
    CHECK (playhead->isVisible());
    CHECK (playheadAt (1.5));

    // In the clip itself, it is the song's own position again.
    phazite::movePlayhead (screen.song, beats (1.0));
    CHECK (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.pianoRoll.isPlayheadShowingLinkedClip(); }));
    CHECK (playheadAt (1.0));
}

TEST_CASE ("The piano roll counts bars from the start of the clip it shows")
{
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 32.0, 8.0); // at bar 9 of the song
    screen.select (clip);

    // The view starts on the whole pixel of the song nearest a beat (TimelineEditor::setViewStart()).
    const auto viewStartsAt = [&screen] (double beat)
    {
        const auto& view = screen.pianoRoll.getView();
        return std::abs (view.start.inBeats() - beat) * view.pixelsPerBeat <= 0.5;
    };

    REQUIRE (screen.pianoRoll.getClips() == std::vector { clip });
    CHECK (screen.pianoRoll.getTimeOrigin().inBeats() == doctest::Approx (32.0));
    CHECK (viewStartsAt (32.0)); // it opens at bar 1

    // Moved in the song, the clip still starts at bar 1, and the view stays on its notes.
    phazite::moveClip (screen.song, clip, beats (48.0), screen.tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.pianoRoll.getTimeOrigin().inBeats() == doctest::Approx (48.0));
    CHECK (viewStartsAt (48.0));
}

TEST_CASE ("The piano roll's grid counts from the start of its clip")
{
    // A clip half a beat into the song: its grid lines are half a beat off the song's.
    PianoRollScreen screen;
    screen.select (screen.addClip (0, 0.5, 8.0));

    phazite::GridSettings grid;
    grid.adaptive = false;
    grid.fixedLevel = 9; // a beat
    screen.pianoRoll.setGrid (grid);

    CHECK (screen.pianoRoll.snap (phazite::BeatPosition::fromBeats (2.7), {}).inBeats() == doctest::Approx (2.5));
    CHECK (screen.pianoRoll.snapDown (phazite::BeatPosition::fromBeats (2.4), {}).inBeats() == doctest::Approx (1.5));
}

TEST_CASE ("Selecting a track shows its clip nearest the playhead, or lets the first note start one")
{
    PianoRollScreen screen;
    screen.addClip (0, 0.0, 4.0);
    const auto later = screen.addClip (0, 16.0, 4.0);

    // The playhead is in neither clip: the nearer one shows.
    phazite::movePlayhead (screen.song, beats (13.0));
    screen.select (screen.tracks[0]);
    CHECK (screen.pianoRoll.getClips() == std::vector { later });

    // A track without clips shows from the bar the playhead is in, and a click there starts a clip.
    const auto second = screen.tracks[1];
    screen.select (second);
    CHECK (screen.pianoRoll.getTrack() == second);
    CHECK (screen.pianoRoll.getClips().empty());
    CHECK (screen.pianoRoll.getTimeOrigin().inBeats() == doctest::Approx (12.0)); // bar 4, which beat 13 is in

    phazite::test::Mouse mouse (*screen.notes);
    mouse.click (screen.at (14.5));

    const auto clips = phazite::getClips (screen.song, second);
    REQUIRE (clips.size() == 1);
    const auto started = clips.front();
    REQUIRE (phazite::isMidiClip (screen.song, started));
    CHECK (phazite::getClipRange (screen.song, started).getStart().inBeats() == doctest::Approx (12.0));
    CHECK (screen.notesOf (started).size() == 1);
    CHECK (screen.pianoRoll.getClips() == std::vector { started });

    // One undo step takes back the clip with its note.
    phazite::undo (screen.song);
    CHECK (phazite::getClips (screen.song, second).empty());
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

TEST_CASE ("The piano roll paints a clip's notes in its grid, and a track's key in its rows")
{
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 0.0, 4.0);
    screen.select (clip);
    CHECK (screen.pianoRoll.getTrack().isValid());
    const auto grid = screen.notesArea();
    const auto empty = screen.window.paintEverything();

    // Two notes, a beat each, on middle C and the E above: they show in the grid's first two beats, and nothing
    // changes in its last beat.
    phazite::addNote (screen.song, clip, 60, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 100);
    phazite::addNote (screen.song, clip, 64, beats (1.0), phazite::BeatDuration::fromBeats (1.0), 100);
    phazite::test::dispatchPendingMessages();
    const auto withNotes = screen.window.paintEverything();
    const auto x = [&screen, grid] (double beat) { return grid.getX() + screen.at (beat).x; };
    CHECK (countChanged (empty, withNotes, grid.withX (x (0.0)).withRight (x (2.0))) > 0);
    CHECK (countChanged (empty, withNotes, grid.withX (x (3.0) + 2).withRight (x (4.0) - 2)) == 0);

    // A key, F sharp major, whose notes are not the white keys': the rows out of it shade otherwise, across the grid.
    phazite::setTrackKey (screen.song, screen.tracks[0], phazite::Key { 6, phazite::Scale::major });
    phazite::test::dispatchPendingMessages();
    const auto withKey = screen.window.paintEverything();
    CHECK (countChanged (withNotes, withKey, grid.withX (x (3.0) + 2).withRight (x (4.0) - 2)) > 0);
}

TEST_CASE ("On a track without an instrument, the piano roll asks for one, since its notes make no sound")
{
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 0.0, 4.0);
    screen.select (clip);

    auto* button = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (screen.pianoRoll, "Instrument"));
    REQUIRE (button != nullptr);
    CHECK (button->getButtonText() == juce::String ("Choose instrument"));

    const auto synth = phazite::test::setOwnInstrument (screen.song, screen.tracks[0], phazite::devices::Synth::typeID);
    REQUIRE (synth.isValid());
    phazite::test::dispatchPendingMessages();
    CHECK (button->getButtonText() == phazite::getDeviceName (screen.song, synth));
}

TEST_CASE ("A click past its clip, where the next clip on the track starts, adds no note there and doesn't stretch the clip over it")
{
    PianoRollScreen screen;
    const auto first = screen.addClip (0, 0.0, 4.0);
    const auto next = screen.addClip (0, 4.0, 4.0);
    screen.select (first);
    const auto undoBefore = phazite::describeUndo (&screen.song);

    // After the clip shown, which can't grow there: a note there wouldn't play, and the piano roll shows the notes that
    // play alone, so none is added, and no undo step.
    phazite::test::Mouse mouse (*screen.notes);
    mouse.click (screen.at (4.5));

    CHECK (screen.notesOf (first).empty());
    CHECK (phazite::getClipRange (screen.song, first).getEnd().inBeats() == doctest::Approx (4.0));
    CHECK (phazite::getClipRange (screen.song, next).getStart().inBeats() == doctest::Approx (4.0));
    CHECK (screen.notesOf (next).empty());
    CHECK (phazite::describeUndo (&screen.song) == undoBefore);

    // In the clip, a click adds its note as before, and the clip stays as long as it was.
    mouse.click (screen.at (3.6));
    REQUIRE (screen.notesOf (first).size() == 1);
    CHECK (phazite::getNotesPlayed (screen.context, { first }).size() == 1);
    CHECK (phazite::getClipRange (screen.song, first).getEnd().inBeats() == doctest::Approx (4.0));
    phazite::undo (screen.song); // one step
    CHECK (screen.notesOf (first).empty());
}

TEST_CASE ("A note added and dragged is one undo step, however long the drag takes")
{
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 0.0, 8.0);
    screen.select (clip);

    phazite::test::Mouse mouse (*screen.notes);
    mouse.press (screen.at (1.0));
    REQUIRE (screen.notesOf (clip).size() == 1);
    mouse.dragTo (screen.at (2.0));
    phazite::test::settle (500); // a pause, with the mouse still down: time itself, which the drag's timers (JUCE's) see pass
    mouse.dragTo (screen.at (3.0));
    mouse.release();

    CHECK (screen.notesOf (clip).front().start.inBeats() > 2.5);
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Add Note"));
    phazite::undo (screen.song);
    CHECK (screen.notesOf (clip).empty()); // the note and all its moves, in one step
}

TEST_CASE ("While a note is dragged, the piano roll says its pitch and where it is, counted from its clip as the ruler counts")
{
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 4.0, 8.0); // its bar 1 is the song's bar 2
    screen.select (clip);
    CHECK (screen.pianoRoll.getDragReadout().isEmpty());

    // A note added at the clip's beat 1 and dragged to its beat 3: the clip's bar 1, beat 4.
    phazite::test::Mouse mouse (*screen.notes);
    mouse.press (screen.at (5.0));
    mouse.dragTo (screen.at (6.0));
    mouse.dragTo (screen.at (7.0));
    REQUIRE (screen.notesOf (clip).size() == 1);
    const auto pitch = screen.notesOf (clip).front().pitch;
    CHECK (screen.pianoRoll.getDragReadout() == juce::MidiMessage::getMidiNoteName (pitch, true, true, 4) + " at 1.4.1");
    mouse.release();
    CHECK (screen.pianoRoll.getDragReadout().isEmpty());
}

TEST_CASE ("The mouse adds, selects, copies, resizes and deletes notes, each change one undo step that keeps the notes' IDs")
{
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 0.0, 8.0);
    screen.select (clip);
    screen.pianoRoll.zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (8.0) });

    auto& context = screen.context;
    const auto notes = [&context, clip] { return phazite::getNotes (context, { clip }); };
    const auto selected = [&context] { return context.getSongSelection().getNotes(); };
    const auto isSelected = [&selected] (phazite::NoteRef note)
    {
        const auto all = selected();
        return std::find (all.begin(), all.end(), note) != all.end();
    };

    phazite::test::Mouse mouse (*screen.notes);

    // Clicks add notes, each selected alone.
    mouse.click (screen.at (1.1));
    mouse.click (screen.at (3.1));
    REQUIRE (notes().size() == 2);
    const auto first = notes()[0].getRef(), second = notes()[1].getRef();
    CHECK (selected() == std::vector { second });
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Add Note"));

    // Ctrl+click adds the first to the selection.
    const auto inside = [&context] (phazite::NoteRef note)
    {
        const auto data = *phazite::getNote (context, note);
        return data.start.inBeats() + data.length.inBeats() / 2.0; // its middle, between its edges' zones
    };

    mouse.click (screen.at (inside (first)), juce::ModifierKeys::commandModifier);
    CHECK (isSelected (first));
    CHECK (isSelected (second));

    // Ctrl+drag copies both, four beats later, as the playlist copies clips; the copies are selected, and the originals stay.
    mouse.press (screen.at (inside (first)), juce::ModifierKeys::commandModifier);
    mouse.dragTo (screen.at (inside (first) + 4.0));
    mouse.release();
    mouse.pause();
    REQUIRE (notes().size() == 4);
    CHECK (selected().size() == 2);
    CHECK_FALSE (isSelected (first));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Copy Notes"));
    CHECK (phazite::getNote (context, first)->start.inBeats() < 1.5); // where it was

    // Dragging the end of the first note makes it longer.
    const auto end = phazite::getNote (context, first)->getEnd().inBeats();
    mouse.press (screen.at (end - 0.02));
    mouse.dragTo (screen.at (2.5));
    mouse.release();
    mouse.pause();
    CHECK (phazite::getNote (context, first)->getEnd().inBeats() > 2.2);
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Resize Notes"));

    // A right-click deletes a note, and Undo puts the same note back.
    mouse.click (screen.at (inside (second)), juce::ModifierKeys::rightButtonModifier);
    CHECK_FALSE (phazite::getNote (context, second).has_value());
    CHECK (notes().size() == 3);
    phazite::undo (screen.song);
    CHECK (phazite::getNote (context, second).has_value());
}

namespace
{
// A screen with a clip of 8 beats shown, zoomed to show it across the grid, with the mouse in the grid and a few of the
// notes' helpers.
struct NotesScreen : PianoRollScreen
{
    NotesScreen()
    {
        clip = addClip (0, 0.0, 8.0);
        select (clip);
        pianoRoll.zoomTo ({ phazite::BeatPosition(), beats (8.0) });
    }

    std::vector<phazite::NoteData> getNotes() const { return phazite::getNotes (context, { clip }); }
    std::vector<phazite::NoteRef> getSelected() const { return context.getSongSelection().getNotes(); }
    phazite::NoteData get (phazite::NoteRef note) const { return *phazite::getNote (context, note); }

    // The middle of a note, between its edges' zones.
    juce::Point<int> middleOf (phazite::NoteRef note) const
    {
        const auto data = get (note);
        return at (data.start.inBeats() + data.length.inBeats() / 2.0);
    }

    phazite::ClipID clip;
    phazite::test::Mouse mouse { *notes };
};
} // namespace

TEST_CASE ("Ctrl+drag copies the selected notes and the one pressed, once however long the drag; Shift selects and moves")
{
    NotesScreen screen;
    auto& mouse = screen.mouse;
    mouse.click (screen.at (1.1));
    mouse.click (screen.at (3.1));
    REQUIRE (screen.getNotes().size() == 2);
    const auto first = screen.getNotes()[0].getRef(), second = screen.getNotes()[1].getRef();
    REQUIRE (screen.getSelected() == std::vector { second });

    // Ctrl+Shift+drag from the first, unselected, copies it with the selected one, once, while the mouse rests too: the
    // drag's repeats once copied them again and again (2026-11-06).
    mouse.press (screen.middleOf (first), juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
    mouse.dragTo (screen.middleOf (first) + juce::Point<int> (screen.at (4.0).x - screen.at (0.0).x, 0));
    phazite::test::settle (200); // the mouse resting: time itself, in which the drag's timers (JUCE's) repeat it
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() == 4);
    CHECK (screen.getSelected().size() == 2);
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Copy Notes"));
    phazite::undo (screen.song);
    REQUIRE (screen.getNotes().size() == 2);

    // Ctrl+drag from an unselected note does the same.
    mouse.click (screen.middleOf (second));
    mouse.press (screen.middleOf (first), juce::ModifierKeys::commandModifier);
    mouse.dragTo (screen.middleOf (first) + juce::Point<int> (screen.at (4.0).x - screen.at (0.0).x, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() == 4);
    phazite::undo (screen.song);
    REQUIRE (screen.getNotes().size() == 2);

    // Shift+click adds a note to the selection or takes it out, as in the playlist; Shift+drag moves them, copying nothing.
    mouse.click (screen.middleOf (second));
    mouse.click (screen.middleOf (first), juce::ModifierKeys::shiftModifier);
    CHECK (screen.getSelected().size() == 2);
    const auto startBefore = screen.get (first).start.inBeats();
    mouse.press (screen.middleOf (first), juce::ModifierKeys::shiftModifier);
    mouse.dragTo (screen.middleOf (first) + juce::Point<int> (screen.at (1.0).x - screen.at (0.0).x, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() == 2);
    CHECK (screen.get (first).start.inBeats() == doctest::Approx (startBefore + 1.0));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Move Notes"));

    // Shift+drag on empty grid adds one note, and moves it.
    mouse.press (screen.at (6.1), juce::ModifierKeys::shiftModifier);
    mouse.dragTo (screen.at (7.1));
    mouse.release();
    CHECK (screen.getNotes().size() == 3);
}

TEST_CASE ("The settings show what a click adds: the note pressed last's length and velocity, which deleting doesn't change")
{
    NotesScreen screen;
    auto& mouse = screen.mouse;
    auto* button = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (screen.pianoRoll, "Next note"));
    REQUIRE (button != nullptr);
    CHECK (button->getButtonText() == juce::String ("1/4 beat, velocity 100"));

    // A note made longer and quieter, then pressed: the next click adds one like it.
    mouse.click (screen.at (1.1));
    const auto note = screen.getNotes().front().getRef();
    phazite::setNoteLength (screen.context, note, phazite::BeatDuration::fromBeats (2.0));
    phazite::setNoteVelocities (screen.context, { note }, 40);
    mouse.click (screen.middleOf (note));
    CHECK (screen.pianoRoll.getNextNoteLength().inBeats() == doctest::Approx (2.0));
    CHECK (screen.pianoRoll.getNextNoteVelocity() == 40);
    CHECK (button->getButtonText() == juce::String ("2 beats, velocity 40"));

    // Another note, deleted with the right button: what a click adds stays.
    const auto other =
        phazite::addNote (screen.context, screen.clip, screen.get (note).pitch, beats (5.0), phazite::BeatDuration::fromBeats (0.5), 90);
    mouse.click (screen.middleOf (other), juce::ModifierKeys::rightButtonModifier);
    CHECK_FALSE (phazite::getNote (screen.context, other).has_value());
    CHECK (screen.pianoRoll.getNextNoteLength().inBeats() == doctest::Approx (2.0));
    CHECK (screen.pianoRoll.getNextNoteVelocity() == 40);

    // A right-click on empty grid clears the selection, and deletes nothing.
    REQUIRE_FALSE (screen.getSelected().empty());
    mouse.click (screen.at (7.5), juce::ModifierKeys::rightButtonModifier);
    CHECK (screen.getSelected().empty());
    CHECK (screen.getNotes().size() == 1);
}

TEST_CASE ("The piano roll's settings show down its left, and its velocity lane folds to a thin row and sizes by its edge")
{
    // The owner's UX audit, 2026-11-12: the toolbar along the top was too narrow for its settings at the default window size,
    // and took a row the notes need; the velocity lane folds, as every row does, by its chevron.
    using phazite::ui::PianoRoll;
    PianoRollScreen screen;
    auto& roll = screen.pianoRoll;

    // Down the left, from the top, each whole and left of the keys: the instrument, the key, what a click adds, the chord stamp
    // and ghost notes.
    auto lastBottom = 0;

    for (const auto* title : { "Instrument", "Key", "Next note", "Chord stamp", "Ghost notes" })
    {
        CAPTURE (title);
        auto* button = phazite::test::findComponentWithTitle (roll, title);
        REQUIRE (button != nullptr);
        CHECK (button->isShowing());
        const auto bounds = roll.getLocalArea (button, button->getLocalBounds());
        CHECK (bounds.getY() >= lastBottom);
        CHECK (bounds.getRight() < screen.notesArea().getX());
        CHECK (bounds.getBottom() <= roll.getHeight());
        lastBottom = bounds.getBottom();
    }

    // Folded by its chevron, the lane is a thin row and the notes take its room; the settings hear of it. Shown again, as it was.
    std::vector<std::pair<int, bool>> heard;
    roll.onVelocityLaneChanged = [&heard] (int height, bool folded) { heard.emplace_back (height, folded); };
    const auto notesHeight = screen.notesArea().getHeight();
    auto* chevron = phazite::test::findComponentWithTitle (roll, "Fold the lane");
    REQUIRE (chevron != nullptr);
    const auto chevronCentre = roll.getLocalArea (chevron, chevron->getLocalBounds()).getCentre();
    phazite::test::Mouse mouse (roll);
    mouse.click (chevronCentre);
    CHECK (roll.isVelocityLaneFolded());
    CHECK (roll.getVelocityLaneHeight() == PianoRoll::foldedVelocityHeight);
    CHECK (screen.notesArea().getHeight() == notesHeight + PianoRoll::defaultVelocityHeight - PianoRoll::foldedVelocityHeight);
    REQUIRE (heard.size() == 1);
    CHECK (heard.back() == std::pair { PianoRoll::defaultVelocityHeight, true });

    mouse.click (roll.getLocalArea (chevron, chevron->getLocalBounds()).getCentre());
    CHECK_FALSE (roll.isVelocityLaneFolded());
    CHECK (screen.notesArea().getHeight() == notesHeight);

    // Its edge dragged up makes the lane taller by as much and the notes shorter; a double-click puts it back.
    auto* edge = phazite::test::findComponentWithTitle (roll, "Velocity lane's edge");
    REQUIRE (edge != nullptr);
    const auto edgeCentre = roll.getLocalArea (edge, edge->getLocalBounds()).getCentre();
    mouse.press (edgeCentre);
    mouse.dragTo (edgeCentre - juce::Point<int> (0, 40));
    mouse.release();
    CHECK (roll.getVelocityLaneHeight() == PianoRoll::defaultVelocityHeight + 40);
    CHECK (screen.notesArea().getHeight() == notesHeight - 40);
    CHECK (heard.back() == std::pair { PianoRoll::defaultVelocityHeight + 40, false });

    const auto movedEdge = roll.getLocalArea (edge, edge->getLocalBounds()).getCentre();
    mouse.doubleClick (movedEdge);
    CHECK (roll.getVelocityLaneHeight() == PianoRoll::defaultVelocityHeight);
    CHECK (screen.notesArea().getHeight() == notesHeight);
}

TEST_CASE ("Each frame of a drag of the velocity lane's edge shows what a fresh paint shows: no name left behind where the lane was")
{
    // The owner, 2026-11-19: "dragging the velocity track larger or smaller creates a visual glitch with the velocity text". The
    // piano roll paints the lane's corner under the keys with its name, and the line over it; nothing that moved with the lane
    // repainted where they had been, so the name stayed there, drawn twice.
    PianoRollScreen screen;
    auto& roll = screen.pianoRoll;
    screen.select (screen.addClip (0, 0.0, 8.0));
    auto* edge = phazite::test::findComponentWithTitle (roll, "Velocity lane's edge");
    REQUIRE (edge != nullptr);

    // What the screen shows: the last frame, with what was asked to repaint since painted again over it, as a window keeps it.
    const juce::SoftwareImageType software;
    auto frame = screen.window.paintEverything (software);
    screen.window.takeRepaintedArea();

    const auto differingPixels = [] (const juce::Image& a, const juce::Image& b)
    {
        REQUIRE (a.getBounds() == b.getBounds());
        const juce::Image::BitmapData first (a, juce::Image::BitmapData::readOnly), second (b, juce::Image::BitmapData::readOnly);
        auto count = 0;

        for (int y = 0; y < a.getHeight(); ++y)
            for (int x = 0; x < a.getWidth(); ++x)
                count += first.getPixelColour (x, y) != second.getPixelColour (x, y) ? 1 : 0;

        return count;
    };

    const auto edgeCentre = roll.getLocalArea (edge, edge->getLocalBounds()).getCentre();
    phazite::test::Mouse mouse (roll);
    mouse.press (edgeCentre);

    // Taller in steps, then shorter in steps past where it began.
    for (const auto offset : { -6, -13, -25, -40, -31, -18, -4, 9, 22, 30 })
    {
        CAPTURE (offset);
        mouse.dragTo (edgeCentre + juce::Point<int> (0, offset));

        {
            juce::Graphics g (frame);
            g.reduceClipRegion (screen.window.takeRepaintedArea());
            screen.window.paintEntireComponent (g, true);
        }

        CHECK (differingPixels (frame, screen.window.paintEverything (software)) == 0);
    }

    mouse.release();
}

TEST_CASE ("A clip opens zoomed to fit with room after it, never smaller than the usual zoom, and opens again as it was left")
{
    PianoRollScreen screen;
    const auto bar = screen.addClip (0, 0.0, 4.0);
    const auto longClip = screen.addClip (1, 0.0, 64.0);
    const auto& view = screen.pianoRoll.getView();
    const auto beatsShown = [&screen, &view] { return screen.notesArea().getWidth() / view.pixelsPerBeat; };

    // A bar shows with another after it to draw in: a quarter-beat note is wide enough to grab.
    screen.select (bar);
    CHECK (beatsShown() == doctest::Approx (8.0).epsilon (0.02));

    // A long clip shows its start at the editors' usual zoom.
    screen.select (longClip);
    CHECK (view.pixelsPerBeat >= phazite::ui::TimelineView::defaultPixelsPerBeat - 1.0e-9);
    CHECK (view.start.inBeats() == doctest::Approx (0.0));

    // Zoomed in on the bar, it opens so again.
    screen.select (bar);
    screen.pianoRoll.zoomTo ({ beats (1.0), beats (3.0) });
    const auto zoomed = view.pixelsPerBeat;
    screen.select (longClip);
    screen.select (bar);
    CHECK (view.pixelsPerBeat == doctest::Approx (zoomed));
    CHECK (view.start.inBeats() == doctest::Approx (1.0));
}

TEST_CASE ("Linked clips show their notes as any of them was left: the same zoom, the same notes in view and the same rows")
{
    // The owner, 2026-11-13: "the scroll positions of the piano roll should be the same for all linked [clips] when any of
    // them are the selected item". An alias plays the same notes, so it shows them where its link was left.
    PianoRollScreen screen;
    const auto clip = screen.addClip (0, 0.0, 4.0);
    phazite::addNote (screen.song, clip, 60, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 100);
    const auto alias = phazite::copyClips (screen.song, { clip }, true).front();
    phazite::moveClip (screen.context, alias, beats (8.0), screen.tracks[0]);
    const auto other = screen.addClip (1, 16.0, 4.0);
    CHECK (phazite::getLinkedClips (screen.context, alias) == std::vector { alias, clip });
    CHECK (phazite::getLinkedClips (screen.context, other) == std::vector { other });

    // Left zoomed in and scrolled down in the first.
    const auto& view = screen.pianoRoll.getView();
    screen.select (clip);
    screen.pianoRoll.zoomTo ({ beats (1.0), beats (3.0) });
    screen.pianoRoll.setScrollY (screen.pianoRoll.getScrollY() + 120);
    const auto zoomed = view.pixelsPerBeat;
    const auto scrolled = screen.pianoRoll.getScrollY();

    // The alias, never shown, opens so, from its own bar 1.
    screen.select (alias);
    CHECK (screen.pianoRoll.getTimeOrigin() == beats (8.0));
    CHECK (view.pixelsPerBeat == doctest::Approx (zoomed));
    CHECK ((view.start - screen.pianoRoll.getTimeOrigin()).inBeats() == doctest::Approx (1.0));
    CHECK (screen.pianoRoll.getScrollY() == scrolled);

    // Scrolled further there, the first opens as the alias was left, past a clip of its own.
    screen.pianoRoll.setScrollY (scrolled - 60);
    screen.select (other);
    screen.select (clip);
    CHECK (view.start.inBeats() == doctest::Approx (1.0));
    CHECK (screen.pianoRoll.getScrollY() == scrolled - 60);
}

TEST_CASE ("A short note's edges are grabbed just outside it, and a long note's start as easily as its end")
{
    NotesScreen screen;
    auto& mouse = screen.mouse;
    const auto& view = screen.pianoRoll.getView();
    const auto xOf = [&view] (phazite::BeatPosition beat) { return juce::roundToInt (view.beatToX (beat)); };

    screen.pianoRoll.zoomTo ({ phazite::BeatPosition(), beats (64.0) }); // a quarter-beat note is under 4 pixels wide
    mouse.click (screen.at (4.05));                                      // at the bar the coarse grid gives there
    REQUIRE (screen.getNotes().size() == 1);
    const auto note = screen.getNotes().front().getRef();
    const auto y = screen.at (0.0).y;
    const auto startBefore = screen.get (note).start;
    REQUIRE (view.beatToX (screen.get (note).getEnd()) - view.beatToX (startBefore) < 5.0);

    // Two pixels past its end, a drag makes it longer (with Alt, off the grid).
    const auto endX = xOf (screen.get (note).getEnd());
    mouse.press ({ endX + 2, y }, juce::ModifierKeys::altModifier);
    mouse.dragTo ({ endX + 2 + juce::roundToInt (view.pixelsPerBeat), y });
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() == 1); // no note added there
    CHECK (screen.get (note).length.inBeats() > 1.0);
    CHECK (screen.get (note).start == startBefore);

    // Zoomed in, the note is long: three pixels inside its start, a drag moves its start and keeps its end.
    screen.pianoRoll.zoomTo ({ phazite::BeatPosition(), beats (8.0) });
    const auto end = screen.get (note).getEnd();
    const auto startX = xOf (screen.get (note).start);
    mouse.press ({ startX + 3, y }, juce::ModifierKeys::altModifier);
    mouse.dragTo ({ startX + 3 - juce::roundToInt (view.pixelsPerBeat), y });
    mouse.release();
    CHECK (screen.get (note).start.inBeats() < startBefore.inBeats() - 0.5);
    CHECK (screen.get (note).getEnd().inBeats() == doctest::Approx (end.inBeats()));
}

TEST_CASE ("Escape during a drag takes back all it did, and otherwise is the commands' key, which deselects the notes")
{
    NotesScreen screen;
    auto& mouse = screen.mouse;
    const auto escape = juce::KeyPress (juce::KeyPress::escapeKey);
    mouse.click (screen.at (1.1));
    const auto note = screen.getNotes().front().getRef();
    const auto startBefore = screen.get (note).start.inBeats();

    // A note dragged three beats along, then Escape: back where it was, with nothing to undo or redo of the drag, and the
    // mouse does nothing more until it goes up.
    mouse.press (screen.middleOf (note));
    mouse.dragTo (screen.middleOf (note) + juce::Point<int> (screen.at (3.0).x - screen.at (0.0).x, 0));
    REQUIRE (screen.get (note).start.inBeats() > startBefore + 2.0);
    CHECK (screen.pianoRoll.keyPressed (escape));
    CHECK (screen.get (note).start.inBeats() == doctest::Approx (startBefore));
    mouse.dragTo (screen.at (6.0));
    mouse.release();
    mouse.pause();
    CHECK (screen.get (note).start.inBeats() == doctest::Approx (startBefore));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Add Note"));
    CHECK_FALSE (screen.song.getUndoManager().canRedo());

    // A note a press added goes with Escape before the mouse goes up.
    mouse.press (screen.at (5.1));
    REQUIRE (screen.getNotes().size() == 2);
    CHECK (screen.pianoRoll.keyPressed (escape));
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() == 1);

    // A velocity drag too: from the selected note's bar, down to the lane's foot.
    REQUIRE (screen.getSelected() == std::vector { note });
    auto* lane = phazite::test::findComponentWithTitle (screen.pianoRoll, "Velocity");
    REQUIRE (lane != nullptr);
    phazite::test::Mouse laneMouse (*lane);
    const auto barX = juce::roundToInt (screen.pianoRoll.getView().beatToX (screen.get (note).start));
    laneMouse.press ({ barX, 10 });
    laneMouse.dragTo ({ barX, lane->getHeight() - 3 });
    REQUIRE (screen.get (note).velocity < 20);
    CHECK (screen.pianoRoll.keyPressed (escape));
    laneMouse.release();
    CHECK (screen.get (note).velocity == 100);

    // Without a drag, Escape isn't the piano roll's: the keymaps give it to Deselect All Notes (after Cancel Transform).
    mouse.click (screen.middleOf (note));
    REQUIRE_FALSE (screen.getSelected().empty());
    CHECK_FALSE (screen.pianoRoll.keyPressed (escape));
    screen.pianoRoll.setActive (true);
    REQUIRE (screen.registry.invoke ("note.deselectAll"));
    CHECK (screen.getSelected().empty());
}

TEST_CASE ("A click on a key selects its notes, Ctrl+click adds them, and a drag along the keys selects their range")
{
    NotesScreen screen;
    auto* keys = phazite::test::findComponentWithTitle (screen.pianoRoll, "Keys");
    REQUIRE (keys != nullptr);

    // A note in the grid's middle row, and others on it and two rows up; the keys' middle is that row.
    screen.mouse.click (screen.at (1.1));
    const auto pitch = screen.getNotes().front().pitch;
    const auto again = phazite::addNote (screen.context, screen.clip, pitch, beats (4.0), phazite::BeatDuration::fromBeats (1.0), 100);
    const auto higher = phazite::addNote (screen.context, screen.clip, pitch + 2, beats (2.0), phazite::BeatDuration::fromBeats (1.0), 100);
    const auto middle = juce::Point<int> (keys->getWidth() / 4, screen.at (0.0).y);
    phazite::test::Mouse mouse (*keys);

    mouse.click (middle);
    auto selected = screen.getSelected();
    CHECK (selected.size() == 2);
    CHECK (std::find (selected.begin(), selected.end(), again) != selected.end());

    // Ctrl+click on the key two rows up adds its note. (The rows' scroll bar spans the 128 rows.)
    const auto rowHeight = [&screen]
    {
        for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.pianoRoll))
            if (bar->isVertical() && bar->findParentComponentOfClass<juce::Viewport>() == nullptr) // not the settings'
                return juce::roundToInt (bar->getMaximumRangeLimit() / 128.0);

        return 0;
    }();

    REQUIRE (rowHeight > 0);
    mouse.click (middle - juce::Point<int> (0, 2 * rowHeight), juce::ModifierKeys::commandModifier);
    CHECK (screen.getSelected().size() == 3);

    // A drag from the middle up two keys selects the notes of the three keys, and nothing else: a plain press selects only
    // its key's.
    mouse.press (middle);
    CHECK (screen.getSelected().size() == 2);
    mouse.dragTo (middle - juce::Point<int> (0, 2 * rowHeight));
    mouse.release();
    selected = screen.getSelected();
    CHECK (selected.size() == 3);
    CHECK (std::find (selected.begin(), selected.end(), higher) != selected.end());
}

TEST_CASE ("Velocity shows on the notes; in the lane, a selected note's bar moves every selected one by as much, the right "
           "button draws a ramp, and the wheel raises or lowers it")
{
    NotesScreen screen;
    std::vector<phazite::NoteRef> notes;

    for (const auto& [beat, velocity] : { std::pair { 1.0, 100 }, std::pair { 3.0, 80 }, std::pair { 5.0, 60 } })
        notes.push_back (
            phazite::addNote (screen.context, screen.clip, 60, beats (beat), phazite::BeatDuration::fromBeats (1.0), velocity));

    const auto velocityOf = [&screen] (phazite::NoteRef note) { return screen.get (note).velocity; };
    auto* lane = phazite::test::findComponentWithTitle (screen.pianoRoll, "Velocity");
    REQUIRE (lane != nullptr);
    phazite::test::Mouse mouse (*lane);
    const auto barX = [&screen] (phazite::NoteRef note)
    { return juce::roundToInt (screen.pianoRoll.getView().beatToX (screen.get (note).start)); };
    const auto yFor = [lane] (int velocity)
    { return juce::roundToInt (2.0f + ((float) lane->getHeight() - 4.0f) * (1.0f - (float) velocity / 127.0f)); };

    // A loud note and a quiet one look different, in the middle of each (away from its name and its velocity's bar). Middle
    // C's row is in the middle of the grid, which shows it with no notes yet.
    phazite::setNoteVelocities (screen.context, { notes[2] }, 1);
    {
        const auto image = screen.notes->createComponentSnapshot (screen.notes->getLocalBounds());
        const auto rowMiddle = screen.at (0.0).y + 5;
        CHECK (image.getPixelAt (screen.middleOf (notes[0]).x, rowMiddle) != image.getPixelAt (screen.middleOf (notes[2]).x, rowMiddle));
    }

    phazite::setNoteVelocities (screen.context, { notes[2] }, 60);

    // All three selected: a drag from the first's bar lowers each by as much, keeping their differences.
    screen.pianoRoll.setActive (true);
    REQUIRE (screen.registry.invoke ("note.selectAll"));
    mouse.press ({ barX (notes[0]), yFor (100) });
    mouse.dragTo ({ barX (notes[0]), yFor (80) });
    mouse.release();
    mouse.pause();
    CHECK (velocityOf (notes[0]) - velocityOf (notes[1]) == 20);
    CHECK (velocityOf (notes[1]) - velocityOf (notes[2]) == 20);
    CHECK (velocityOf (notes[0]) == doctest::Approx (80).epsilon (0.05));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Change Velocity"));

    // The right button draws a straight ramp across them, from loud to quiet.
    mouse.press ({ barX (notes[0]), yFor (120) }, juce::ModifierKeys::rightButtonModifier);
    mouse.dragTo ({ barX (notes[2]), yFor (20) });
    mouse.release();
    mouse.pause();
    CHECK (velocityOf (notes[0]) == doctest::Approx (120).epsilon (0.05));
    CHECK (velocityOf (notes[1]) == doctest::Approx (70).epsilon (0.05));
    CHECK (velocityOf (notes[2]) == doctest::Approx (20).epsilon (0.1));

    // The wheel over a note's bar raises it a step each turn.
    const auto before = velocityOf (notes[2]);
    mouse.wheel ({ barX (notes[2]), yFor (60) }, 1.0f);
    CHECK (velocityOf (notes[2]) > before);
}

TEST_CASE ("In the piano roll the Draw Tool's key picks the Smart Tool, which adds notes with a click already")
{
    NotesScreen screen;
    screen.pianoRoll.setActive (true);
    REQUIRE (screen.registry.invoke ("tool.draw"));
    CHECK (screen.pianoRoll.getTool() == phazite::ui::Tool::smart);

    auto* draw = phazite::test::findComponentWithTitle (screen.pianoRoll.getToolRow(), "Draw tool");
    REQUIRE (draw != nullptr);
    CHECK_FALSE (draw->isVisible());
}

TEST_CASE ("Paint lays notes either way from where it starts, in the key and selected as a click's note is")
{
    NotesScreen screen;
    auto& mouse = screen.mouse;

    phazite::GridSettings grid;
    grid.adaptive = false;
    grid.fixedLevel = 9; // a beat
    screen.pianoRoll.setGrid (grid);

    // The grid's middle row: its pitch, from a note a click adds there.
    mouse.click (screen.at (0.5));
    const auto pitch = screen.getNotes().front().pitch;
    const auto first = screen.getNotes().front().getRef();

    // A key the middle row is outside: its notes snap to the key, a semitone down.
    phazite::setTrackKey (screen.song, screen.tracks[0], phazite::Key { (pitch + 2) % 12, phazite::Scale::major });
    phazite::test::dispatchPendingMessages();
    screen.pianoRoll.setActive (true);
    REQUIRE (screen.registry.invoke ("pianoRoll.snapToKey"));

    REQUIRE (screen.registry.invoke ("tool.paint"));
    mouse.press (screen.at (4.5));
    mouse.dragTo (screen.at (6.5));
    mouse.dragTo (screen.at (2.5));
    mouse.release();
    mouse.pause();

    std::vector<double> starts;

    for (const auto& note : screen.getNotes())
        if (note.getRef() != first)
        {
            CHECK (note.pitch == pitch - 1);
            starts.push_back (note.start.inBeats());
        }

    std::sort (starts.begin(), starts.end());
    CHECK (starts == std::vector { 2.0, 3.0, 4.0, 5.0, 6.0 });
    CHECK (screen.getSelected().size() == 5); // the painted ones, not the clicked one
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Paint Notes"));
}

TEST_CASE ("Alt+wheel makes the rows taller, as it makes the playlist's tracks, and a middle drag moves the view, adding nothing")
{
    NotesScreen screen;
    juce::ScrollBar* rows = nullptr;

    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (screen.pianoRoll))
        if (bar->isVertical() && bar->findParentComponentOfClass<juce::Viewport>() == nullptr) // not the settings'
            rows = bar;

    REQUIRE (rows != nullptr);
    const auto tallBefore = rows->getMaximumRangeLimit();
    screen.mouse.wheel (screen.at (2.0), 1.0f, juce::ModifierKeys::altModifier);
    CHECK (rows->getMaximumRangeLimit() > tallBefore);
    CHECK (screen.getNotes().empty());

    // The middle button drags the notes along, as a page by hand; it never edits.
    const auto startBefore = screen.pianoRoll.getView().start.inBeats();
    screen.mouse.press (screen.at (4.0), juce::ModifierKeys::middleButtonModifier);
    screen.mouse.dragTo (screen.at (2.0));
    screen.mouse.release();
    CHECK (screen.pianoRoll.getView().start.inBeats() == doctest::Approx (startBefore + 2.0).epsilon (0.02));
    CHECK (screen.getNotes().empty());
}

TEST_CASE ("Strum, humanize and arpeggiate preview in the transform bar and apply as one step; a closed bar leaves the settings "
           "as they were, and Quantize names those it uses")
{
    NotesScreen screen;
    for (const auto pitch : { 60, 64, 67 })
        phazite::addNote (screen.context, screen.clip, pitch, beats (0.0), phazite::BeatDuration::fromBeats (2.0), 100);

    screen.pianoRoll.setActive (true);
    REQUIRE (screen.registry.invoke ("note.selectAll"));

    // Strum: the chord's notes start one after another.
    REQUIRE (screen.registry.invoke ("note.strumSettings"));
    REQUIRE (screen.registry.invoke ("note.applyTransform"));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Strum Notes"));
    std::vector<double> starts;

    for (const auto& note : screen.getNotes())
        starts.push_back (note.start.inBeats());

    std::sort (starts.begin(), starts.end());
    REQUIRE (starts.size() == 3);
    CHECK (starts[1] > starts[0]);
    CHECK (starts[2] > starts[1]);
    phazite::undo (screen.song);
    REQUIRE (screen.registry.invoke ("note.selectAll"));

    // Arpeggiate: a note at each step of the grid for as long as the chord lasts.
    phazite::GridSettings grid;
    grid.adaptive = false;
    grid.fixedLevel = 8; // half a beat
    screen.pianoRoll.setGrid (grid);
    REQUIRE (screen.registry.invoke ("note.arpeggiateSettings"));
    REQUIRE (screen.registry.invoke ("note.applyTransform"));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Arpeggiate Notes"));
    CHECK (screen.getNotes().size() == 4);
    phazite::undo (screen.song);
    REQUIRE (screen.registry.invoke ("note.selectAll"));

    // Humanize: one step.
    REQUIRE (screen.registry.invoke ("note.humanizeSettings"));
    REQUIRE (screen.registry.invoke ("note.applyTransform"));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Humanize Notes"));
    REQUIRE (screen.registry.invoke ("note.selectAll"));

    // Quantize... at half strength, closed without applying: Quantize keeps its plain settings and name.
    const auto* quantizeCommand = screen.registry.find ("edit.quantize");
    REQUIRE (quantizeCommand != nullptr);
    CHECK (phazite::getCurrentName (*quantizeCommand) == juce::String ("Quantize"));
    REQUIRE (screen.registry.invoke ("note.quantizeSettings"));
    auto* strength = dynamic_cast<juce::Slider*> (phazite::test::findComponentWithTitle (screen.pianoRoll, "Strength"));
    REQUIRE (strength != nullptr);
    strength->setValue (50.0, juce::sendNotificationSync);
    REQUIRE (screen.registry.invoke ("note.cancelTransform"));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getCurrentName (*quantizeCommand) == juce::String ("Quantize"));

    // Applied, they stay, and Quantize says so.
    REQUIRE (screen.registry.invoke ("note.quantizeSettings"));
    strength->setValue (50.0, juce::sendNotificationSync);
    REQUIRE (screen.registry.invoke ("note.applyTransform"));
    CHECK (phazite::getCurrentName (*quantizeCommand) == juce::String ("Quantize (50%)"));
}

TEST_CASE ("With Snap on, a move takes notes by the grid's steps from where they were: a note off the grid stays as far off it")
{
    // A drag up or down pulled a note off the grid onto it, and every selected note with it: the start snapped, not the move.
    NotesScreen screen;
    auto& mouse = screen.mouse;
    REQUIRE (screen.pianoRoll.getGrid().snap);
    mouse.click (screen.at (1.1));
    mouse.click (screen.at (3.1));
    REQUIRE (screen.getNotes().size() == 2);
    const auto first = screen.getNotes()[0].getRef(), second = screen.getNotes()[1].getRef();
    const auto pitch = screen.get (first).pitch;
    const auto step = screen.pianoRoll.getGridStepInBeats();
    REQUIRE (step > 0.05);
    const auto offGrid = 1.0 + step * 0.3; // as a note played live is
    phazite::moveNote (screen.context, first, beats (offGrid), pitch);
    phazite::test::dispatchPendingMessages();
    screen.song.getUndoManager().clearUndoHistory();
    mouse.click (screen.middleOf (first));
    mouse.click (screen.middleOf (second), juce::ModifierKeys::shiftModifier);
    REQUIRE (screen.getSelected().size() == 2);

    // Up, and nothing sideways: another pitch at the same time, the other selected note's too.
    mouse.press (screen.middleOf (first));
    mouse.dragTo (screen.middleOf (first) + juce::Point<int> (0, -60));
    mouse.release();
    mouse.pause();
    CHECK (screen.get (first).pitch != pitch);
    CHECK (screen.get (first).start.inBeats() == doctest::Approx (offGrid));
    CHECK (screen.get (second).start.inBeats() == doctest::Approx (3.0));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Move Notes"));

    // Sideways by a beat: a beat later, as far off the grid as it was.
    const auto oneBeat = screen.at (1.0).x - screen.at (0.0).x;
    const auto grabbed = screen.at (offGrid + screen.get (first).length.inBeats() / 2.0) + juce::Point<int> (0, -60);
    mouse.press (grabbed);
    mouse.dragTo (grabbed + juce::Point<int> (oneBeat, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.get (first).start.inBeats() == doctest::Approx (offGrid + 1.0));
    CHECK (screen.get (second).start.inBeats() == doctest::Approx (4.0));

    // With Alt, it goes where the pointer does, as before.
    const auto before = screen.get (first).start.inBeats();
    const auto grabbedAgain = grabbed + juce::Point<int> (oneBeat, 0);
    mouse.press (grabbedAgain, juce::ModifierKeys::altModifier);
    mouse.dragTo (grabbedAgain + juce::Point<int> (7, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.get (first).start.inBeats() == doctest::Approx (before + 7.0 / oneBeat).epsilon (0.001));
}

TEST_CASE ("A Ctrl+click whose pointer slips copies nothing, and a drag that ends where it began is the click, without an undo step")
{
    // Ctrl+click toggles a note, but four pixels of movement began a drag that copied at once: with Snap on the copies stayed
    // on their originals, unseen and playing twice.
    NotesScreen screen;
    auto& mouse = screen.mouse;
    mouse.click (screen.at (1.1));
    mouse.click (screen.at (3.1));
    REQUIRE (screen.getNotes().size() == 2);
    const auto first = screen.getNotes()[0].getRef(), second = screen.getNotes()[1].getRef();
    REQUIRE (screen.getSelected() == std::vector { second });
    const auto isSelected = [&screen] (phazite::NoteRef note)
    {
        const auto all = screen.getSelected();
        return std::find (all.begin(), all.end(), note) != all.end();
    };

    // The pointer slips five pixels during a Ctrl+click: the note joins the selection, as a Ctrl+click's does.
    mouse.press (screen.middleOf (first), juce::ModifierKeys::commandModifier);
    mouse.dragTo (screen.middleOf (first) + juce::Point<int> (5, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() == 2);
    CHECK (isSelected (first));
    CHECK (isSelected (second));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Add Note"));

    // Again: it leaves the selection.
    mouse.press (screen.middleOf (first), juce::ModifierKeys::commandModifier);
    mouse.dragTo (screen.middleOf (first) + juce::Point<int> (5, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() == 2);
    CHECK_FALSE (isSelected (first));
    CHECK (isSelected (second));

    // A Ctrl+drag makes its copies once it moves them; back where it began, they go, and nothing is left to undo or redo.
    const auto fourBeats = screen.at (4.0).x - screen.at (0.0).x;
    const auto pressed = screen.middleOf (first);
    mouse.press (pressed, juce::ModifierKeys::commandModifier);
    mouse.dragTo (pressed + juce::Point<int> (5, 0));
    CHECK (screen.getNotes().size() == 2);
    mouse.dragTo (pressed + juce::Point<int> (fourBeats, 0));
    CHECK (screen.getNotes().size() == 4);
    mouse.dragTo (pressed + juce::Point<int> (2, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() == 2);
    CHECK (isSelected (first)); // the click
    CHECK (isSelected (second));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Add Note"));
    CHECK_FALSE (phazite::canRedo (screen.song));

    // A move there and back leaves no step either.
    const auto start = screen.get (first).start.inBeats();
    mouse.press (pressed);
    mouse.dragTo (pressed + juce::Point<int> (fourBeats, 0));
    CHECK (screen.get (first).start.inBeats() == doctest::Approx (start + 4.0));
    mouse.dragTo (pressed + juce::Point<int> (1, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.get (first).start.inBeats() == doctest::Approx (start));
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Add Note"));
    CHECK_FALSE (phazite::canRedo (screen.song));

    // And a Ctrl+drag that ends elsewhere copies, as one step.
    mouse.press (pressed, juce::ModifierKeys::commandModifier);
    mouse.dragTo (pressed + juce::Point<int> (fourBeats, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getNotes().size() > 2);
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Copy Notes"));
    phazite::undo (screen.song);
    CHECK (screen.getNotes().size() == 2);
}

TEST_CASE ("Notes that a split or a trim left outside a clip aren't shown, selected or changed by edits of those it plays")
{
    // A split's right half keeps every note, and a trimmed clip the notes past its end. Select All took those too: Quantize
    // then put the notes before the clip on its first beat, a nudge or a drag moved everything right by as much, and the
    // notes after it made the clip grow back.
    NotesScreen screen;
    auto& context = screen.context;
    const auto length = phazite::BeatDuration::fromBeats (0.5);

    // On the row the mouse clicks in, halfway down the grid.
    screen.mouse.click (screen.at (5.1));
    REQUIRE (screen.getNotes().size() == 1);
    const auto row = screen.getNotes()[0].pitch;
    phazite::deleteNotes (context, { screen.getNotes()[0].getRef() });

    for (const auto start : { 1.1, 5.1, 7.0 })
        REQUIRE (phazite::addNote (context, screen.clip, row, beats (start), length, 100).isValid());

    const auto halves = phazite::splitClips (context, { screen.clip }, beats (4.0));
    REQUIRE (halves.size() == 1);
    const auto right = halves[0];
    phazite::trimClipEnd (context, right, beats (6.0));
    REQUIRE (phazite::getNoteClip (context, right)->range == phazite::BeatRange (beats (4.0), beats (6.0)));
    screen.select (right);
    screen.pianoRoll.zoomTo ({ beats (4.0), beats (8.0) });
    screen.pianoRoll.setActive (true);
    screen.song.getUndoManager().clearUndoHistory();

    // What the clip holds, by where each note starts in the song, in that order.
    const auto sameStarts = [&screen, right] (std::vector<double> expected)
    {
        std::vector<double> starts;

        for (const auto& note : screen.notesOf (right))
            starts.push_back (note.start.inBeats());

        std::sort (starts.begin(), starts.end());
        return starts.size() == expected.size()
               && std::equal (starts.begin(), starts.end(), expected.begin(),
                              [] (double a, double b) { return std::abs (a - b) < 1.0e-6; });
    };

    REQUIRE (sameStarts ({ 1.1, 5.1, 7.0 }));
    const auto played = phazite::getNotesPlayed (context, { right });
    REQUIRE (played.size() == 1);
    const auto note = played[0].getRef();

    // Select All takes the note the clip plays, alone.
    REQUIRE (screen.registry.invoke ("note.selectAll"));
    CHECK (screen.getSelected() == std::vector { note });

    // Quantize moves it onto the grid, and the others stay where they are.
    const auto step = screen.pianoRoll.getGridStepInBeats();
    REQUIRE (step > 0.05);
    const auto onGrid = std::round (5.1 / step) * step;
    REQUIRE (screen.registry.invoke ("edit.quantize"));
    CHECK (sameStarts ({ 1.1, onGrid, 7.0 }));

    // A nudge moves it a step, and the clip stays as long as it was trimmed.
    REQUIRE (screen.registry.invoke ("note.selectAll"));
    REQUIRE (screen.registry.invoke ("edit.nudgeRight"));
    CHECK (sameStarts ({ 1.1, onGrid + step, 7.0 }));
    CHECK (phazite::getNoteClip (context, right)->range == phazite::BeatRange (beats (4.0), beats (6.0)));
    REQUIRE (screen.registry.invoke ("edit.nudgeLeft"));
    CHECK (sameStarts ({ 1.1, onGrid, 7.0 }));

    // A drag up moves it alone, and not in time.
    const auto dragUp = [&screen, &length] (double start, int above)
    {
        phazite::test::dispatchPendingMessages();
        const auto middle = screen.at (start + length.inBeats() / 2.0) - juce::Point<int> (0, above);
        screen.mouse.press (middle);
        screen.mouse.dragTo (middle + juce::Point<int> (0, -60));
        screen.mouse.release();
        screen.mouse.pause();
    };

    dragUp (onGrid, 0);
    CHECK (sameStarts ({ 1.1, onGrid, 7.0 }));
    CHECK (phazite::getNote (context, note)->pitch != row);
    CHECK (phazite::getNoteClip (context, right)->range == phazite::BeatRange (beats (4.0), beats (6.0)));

    for (const auto& held : screen.notesOf (right))
        if (! (held.getRef() == note))
            CHECK (held.pitch == row);

    // A note the clip's start cuts, which starts before the clip and plays from its start: it goes no earlier, it doesn't
    // pull the others forward, and a move of the others up or down leaves it where it is.
    const auto cut = phazite::addNote (screen.song, right, row, beats (4.0), length, 100);
    REQUIRE (cut.isValid());
    phazite::trimClipStart (context, right, beats (4.25));
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::getNoteClip (context, right)->range.getStart() == beats (4.25));
    REQUIRE (phazite::getNote (context, cut)->start.inBeats() == doctest::Approx (4.0));
    REQUIRE (phazite::getNotesPlayed (context, { right }).size() == 2);
    REQUIRE (screen.registry.invoke ("note.selectAll"));
    CHECK (screen.getSelected().size() == 2);
    REQUIRE (screen.registry.invoke ("edit.nudgeLeft"));
    CHECK (phazite::getNote (context, cut)->start.inBeats() == doctest::Approx (4.0));
    CHECK (phazite::getNote (context, note)->start.inBeats() == doctest::Approx (onGrid));

    const auto pitch = phazite::getNote (context, note)->pitch;
    dragUp (onGrid, 60);
    CHECK (phazite::getNote (context, note)->pitch != pitch);
    CHECK (phazite::getNote (context, cut)->pitch != row);
    CHECK (phazite::getNote (context, cut)->start.inBeats() == doctest::Approx (4.0));
    CHECK (phazite::getNote (context, note)->start.inBeats() == doctest::Approx (onGrid));

    REQUIRE (screen.registry.invoke ("edit.nudgeRight"));
    CHECK (phazite::getNote (context, cut)->start.inBeats() == doctest::Approx (4.0 + step));
    CHECK (phazite::getNote (context, note)->start.inBeats() == doctest::Approx (onGrid + step));
}

TEST_CASE ("A note dragged or nudged toward the next clip on its track stops where its clip still plays it, and comes back")
{
    // Past its clip's end, where the next clip keeps the clip from growing, the note was in the clip without playing: it
    // vanished from the piano roll, deselected, and only Undo brought it back.
    PianoRollScreen screen;
    const auto first = screen.addClip (0, 0.0, 4.0);
    screen.addClip (0, 4.0, 4.0);
    screen.select (first);
    screen.pianoRoll.zoomTo ({ beats (0.0), beats (8.0) });
    phazite::test::Mouse mouse (*screen.notes);
    mouse.click (screen.at (2.1));
    REQUIRE (screen.notesOf (first).size() == 1);
    const auto note = screen.notesOf (first).front().getRef();
    const auto shown = [&screen, first] { return phazite::getNotesPlayed (screen.context, { first }); };
    const auto isSelected = [&screen, note]
    {
        const auto all = screen.context.getSongSelection().getNotes();
        return std::find (all.begin(), all.end(), note) != all.end();
    };
    const auto middleOf = [&screen] (const phazite::NoteData& data)
    { return screen.at (data.start.inBeats() + data.length.inBeats() / 2.0); };
    const auto oneBeat = screen.at (1.0).x - screen.at (0.0).x;

    // Dragged three beats on, to where the next clip is: it stops just before its clip's end, shown and selected.
    REQUIRE (shown().size() == 1);
    auto middle = middleOf (shown()[0]);
    mouse.press (middle);
    mouse.dragTo (middle + juce::Point<int> (3 * oneBeat, 0));
    mouse.release();
    mouse.pause();
    REQUIRE (shown().size() == 1);
    CHECK (shown()[0].start.inBeats() < 4.0);
    CHECK (shown()[0].start.inBeats() > 3.9);
    CHECK (isSelected());
    CHECK (phazite::getClipRange (screen.song, first).getEnd().inBeats() == doctest::Approx (4.0));

    // And is dragged back from there.
    phazite::test::dispatchPendingMessages();
    const auto there = shown()[0].start.inBeats();
    middle = middleOf (shown()[0]);
    mouse.press (middle);
    mouse.dragTo (middle - juce::Point<int> (2 * oneBeat, 0));
    mouse.release();
    mouse.pause();
    REQUIRE (shown().size() == 1);
    CHECK (shown()[0].start.inBeats() == doctest::Approx (there - 2.0));

    // Nudged on with its key, it stops there too.
    screen.pianoRoll.setActive (true);

    for (int i = 0; i < 40; ++i)
        screen.registry.invoke ("edit.nudgeRight");

    REQUIRE (shown().size() == 1);
    CHECK (shown()[0].start.inBeats() < 4.0);
    CHECK (isSelected());
    REQUIRE (screen.registry.invoke ("edit.nudgeLeft"));
    CHECK (shown()[0].start.inBeats() < 3.99);
}

TEST_CASE ("A drag of selected notes that ends where it began keeps them all selected, and a click on one still selects it alone")
{
    // A drag by less than half a grid step moves nothing: taken for the click, it selected the note pressed alone.
    NotesScreen screen;
    auto& mouse = screen.mouse;
    mouse.click (screen.at (1.1));
    mouse.click (screen.at (3.1));
    REQUIRE (screen.getNotes().size() == 2);
    const auto first = screen.getNotes()[0].getRef(), second = screen.getNotes()[1].getRef();
    mouse.click (screen.middleOf (first), juce::ModifierKeys::shiftModifier);
    REQUIRE (screen.getSelected().size() == 2);
    const auto pressed = screen.middleOf (first);

    // The pointer goes five pixels and the mouse goes up: nothing moved, both stay selected, and no undo step.
    mouse.press (pressed);
    mouse.dragTo (pressed + juce::Point<int> (5, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getSelected().size() == 2);
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Add Note"));

    // There and back likewise.
    mouse.press (pressed);
    mouse.dragTo (pressed + juce::Point<int> (screen.at (4.0).x - screen.at (0.0).x, 0));
    mouse.dragTo (pressed + juce::Point<int> (1, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getSelected().size() == 2);
    CHECK (screen.getNotes().size() == 2);
    CHECK (phazite::describeUndo (&screen.song) == juce::String ("Undo Add Note"));

    // A Shift+press that slips still takes the note out, as its click does; a click without a drag selects one alone.
    mouse.press (pressed, juce::ModifierKeys::shiftModifier);
    mouse.dragTo (pressed + juce::Point<int> (5, 0));
    mouse.release();
    mouse.pause();
    CHECK (screen.getSelected() == std::vector { second });
    mouse.click (screen.middleOf (first), juce::ModifierKeys::shiftModifier);
    REQUIRE (screen.getSelected().size() == 2);
    mouse.click (screen.middleOf (first));
    CHECK (screen.getSelected() == std::vector { first });
}

TEST_CASE ("While the display's frames come, the piano roll's wheel scrolls its rows and its view once a frame")
{
    // As the playlist's (TimelineEditor::scrollViewTo(), scrollRowsTo()): the rows straight where the wheel took them at the
    // next frame, the view gliding there.
    PianoRollScreen screen;
    screen.select (screen.addClip (0, 0.0, 4.0));
    auto& roll = screen.pianoRoll;
    roll.setScrollingAtFrames (true);
    const auto rows = roll.getScrollY();
    const auto start = roll.getView().start;
    phazite::test::Mouse mouse (roll);
    const auto centre = roll.getLocalBounds().getCentre();
    mouse.wheel (centre, -0.1f);
    mouse.wheel (centre, -0.1f, juce::ModifierKeys::shiftModifier);
    CHECK (roll.getScrollY() == rows);
    CHECK (roll.getView().start == start);
    const auto target = roll.getScrollTarget();
    REQUIRE (target > start);

    roll.showFrame (100.0);
    CHECK (roll.getScrollY() > rows);

    for (int frame = 1; frame <= 30; ++frame)
        roll.showFrame (100.0 + frame / 60.0);

    CHECK (roll.getView().start == target);
    CHECK (roll.getSubPixel() == 0.0f);
}
