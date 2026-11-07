// SPDX-License-Identifier: AGPL-3.0-or-later
// The inspector (src/ui/mixer/Inspector.h; roadmap step 2.5), driven as the user would: the properties of what is
// selected (a track, clips, notes, automation points, a section), and the song's with nothing selected; changed from its
// rows, each one undo step, several items at once.
#include "UiTestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/NotesModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/Inspector.h"
#include "ui/theme/DesignTokens.h"

#include <algorithm>

namespace
{
using phazite::BeatPosition;
using phazite::TrackID;

// An open project with two tracks, and the inspector in a window.
struct Screen
{
    Screen()
    {
        phazite::ensureTracks (*song, 2);
        setSong (context, song.get());
        song->getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
    }

    ~Screen() { setSong (context, nullptr); }

    TrackID track (int index) const { return phazite::getTrackIDs (*song)[(size_t) index]; }

    // A label's or a button's text anywhere in the inspector.
    bool shows (const juce::String& text)
    {
        const auto labels = phazite::test::findAll<juce::Label> (inspector);
        const auto buttons = phazite::test::findAll<juce::TextButton> (inspector);
        return std::any_of (labels.begin(), labels.end(), [&text] (juce::Label* label) { return label->getText() == text; })
               || std::any_of (buttons.begin(), buttons.end(),
                               [&text] (juce::TextButton* button) { return button->getButtonText() == text; });
    }

    phazite::Song::Ptr song = phazite::createSong(); // 120 BPM, 4/4
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Inspector inspector { commands, context };
    phazite::test::TestWindow window { inspector, 300, 900 };
};
} // namespace

TEST_CASE ("With nothing selected the inspector shows the song; a selected track shows its properties, which change it")
{
    Screen screen;
    auto& selection = screen.context.getSongSelection();
    selection.clear();
    phazite::test::dispatchPendingMessages();
    CHECK (screen.inspector.getShown() == juce::String ("Song"));
    CHECK (screen.shows ("120 BPM"));
    CHECK (screen.shows ("4/4"));
    CHECK (screen.shows ("C major")); // the key the chord lane's chords are in

    selection.selectOnly (screen.track (1));
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Track"));
    CHECK (screen.shows ("Track"));
    CHECK (screen.shows ("Master"));                                                       // where its sound goes
    CHECK (phazite::test::findComponentWithTitle (screen.inspector, "Volume") != nullptr); // its fader
    CHECK (phazite::test::findComponentWithTitle (screen.inspector, "Automation mode") != nullptr);
    phazite::test::checkChildrenApart (screen.inspector);

    // Typing a name and Enter renames it, as one undo step.
    auto editors = phazite::test::findAll<juce::TextEditor> (screen.inspector);
    REQUIRE (editors.size() == 1);
    editors.getFirst()->setText ("Bass", juce::dontSendNotification);
    REQUIRE (editors.getFirst()->onReturnKey != nullptr);
    editors.getFirst()->onReturnKey();
    CHECK (phazite::getTrackRow (screen.context, screen.track (1))->name == juce::String ("Bass"));
    CHECK (phazite::describeUndo (screen.song.get()).isNotEmpty());

    // The volume fader is the track's.
    auto* volume = dynamic_cast<juce::Slider*> (phazite::test::findComponentWithTitle (screen.inspector, "Volume"));
    REQUIRE (volume != nullptr);
    volume->setValue (0.2, juce::sendNotificationSync);
    CHECK (phazite::getVolume (screen.context, screen.track (1)).getValue() == doctest::Approx (0.2f).epsilon (0.01));
}

TEST_CASE ("A selected clip shows its place, length and whether it plays; a section shows its own properties")
{
    Screen screen;

    // A bar-long MIDI clip at the second bar (beat 4 in 4/4), selected as it is made.
    const auto clip = phazite::addMidiClipAt (screen.context, screen.track (0), BeatPosition::fromBeats (4.0));
    REQUIRE (clip.isValid());
    screen.context.getSongSelection().selectOnly (clip);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Clip"));
    CHECK (screen.shows ("Bar 2"));
    CHECK (screen.shows ("1 bar"));
    CHECK (screen.shows ("MIDI"));
    CHECK (screen.shows ("No"));                                                                       // not an alias
    CHECK_FALSE (phazite::test::findComponentWithTitle (screen.inspector, "Fade shape")->isVisible()); // MIDI clips don't fade

    // Its mute switch.
    auto* muted = phazite::test::findComponentWithTitle (screen.inspector, "Muted");
    REQUIRE (muted != nullptr);
    phazite::test::Mouse mouse (*muted);
    mouse.click (muted->getLocalBounds().getCentre());
    CHECK (phazite::getClip (screen.context, clip)->muted);
    phazite::test::checkChildrenApart (screen.inspector);

    // Its name: none of its own, so the field is empty over its track's; one typed is its own, which the playlist shows over
    // it, as one undo step (2026-11-07).
    auto* name = dynamic_cast<juce::TextEditor*> (phazite::test::findComponentWithTitle (screen.inspector, "Name"));
    REQUIRE (name != nullptr);
    CHECK (name->getText().isEmpty());
    CHECK (name->getTextToShowWhenEmpty() == phazite::getTrackRow (screen.context, screen.track (0))->name + " (the track's)");
    name->setText ("Intro keys", juce::dontSendNotification);
    REQUIRE (name->onReturnKey != nullptr);
    name->onReturnKey();
    CHECK (phazite::getClip (screen.context, clip)->name == juce::String ("Intro keys"));
    CHECK_FALSE (phazite::getClip (screen.context, clip)->namedAfterTrack);
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Rename Clip"));

    // Leaving the field as it shows changes nothing.
    phazite::test::dispatchPendingMessages();
    REQUIRE (name->onFocusLost != nullptr);
    name->onFocusLost();
    CHECK (phazite::getClip (screen.context, clip)->name == juce::String ("Intro keys"));

    // A section: its own page.
    const auto section = phazite::addSectionAt (screen.context, BeatPosition::fromBeats (8.0));
    REQUIRE (section.isValid());
    screen.context.getSongSelection().selectOnly (section);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.inspector.getShown() == juce::String ("Section"));
    CHECK (screen.shows ("Bar 3"));
}

namespace
{
// A field of the inspector, by its title, stepped with the arrow keys or typed into, as the user would.
juce::Slider& findField (Screen& screen, const juce::String& title)
{
    auto* field = dynamic_cast<juce::Slider*> (phazite::test::findComponentWithTitle (screen.inspector, title));
    REQUIRE (field != nullptr);
    return *field;
}

void step (juce::Slider& field, bool up)
{
    field.keyPressed (juce::KeyPress (up ? juce::KeyPress::upKey : juce::KeyPress::downKey));
    phazite::test::dispatchPendingMessages();
}

void type (juce::Slider& field, const juce::String& text)
{
    auto* box = phazite::test::findAll<juce::Label> (field).getFirst();
    REQUIRE (box != nullptr);
    box->setText (text, juce::sendNotificationSync); // as Enter leaves the field's text box
    phazite::test::dispatchPendingMessages();
}
} // namespace

TEST_CASE ("Selected notes show the range of their values, which a step changes for each and typing sets, as one undo step")
{
    Screen screen;
    using phazite::BeatDuration;
    const auto clip = phazite::addMidiClipAt (screen.context, screen.track (0), BeatPosition());
    const auto c4 = phazite::addNote (screen.context, clip, 60, BeatPosition(), BeatDuration::fromBeats (1.0), 64);
    const auto e4 = phazite::addNote (screen.context, clip, 64, BeatPosition::fromBeats (1.0), BeatDuration::fromBeats (0.5), 100);
    screen.song->getUndoManager().clearUndoHistory();
    auto& selection = screen.context.getSongSelection();
    selection.setNoteClips ({ clip });
    selection.selectNotes ({ c4, e4 });
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Notes"));
    CHECK (screen.shows ("2 notes"));
    CHECK (screen.shows ("C4 to E4"));
    CHECK (screen.shows ("64 to 100"));
    phazite::test::checkChildrenApart (screen.inspector);

    const auto noteOf = [&screen] (phazite::NoteRef ref) { return *phazite::getNote (screen.context, ref); };

    // A step up: each note one louder, as one undo step.
    step (findField (screen, "Velocity"), true);
    CHECK (noteOf (c4).velocity == 65);
    CHECK (noteOf (e4).velocity == 101);
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Change Velocity"));
    CHECK (screen.shows ("65 to 101"));

    // Dragged up, each by as much, as one undo step that takes all of the drag back.
    {
        auto& velocity = findField (screen, "Velocity");
        phazite::test::Mouse mouse (velocity);
        const auto centre = velocity.getLocalBounds().getCentre();
        mouse.press (centre);
        mouse.dragTo (centre.translated (0, -10));
        mouse.dragTo (centre.translated (0, -20));
        mouse.release();
        phazite::test::dispatchPendingMessages();
        CHECK (noteOf (c4).velocity > 65);
        CHECK (noteOf (e4).velocity - noteOf (c4).velocity == 36);
        phazite::undo (*screen.song);
        CHECK (noteOf (c4).velocity == 65);
        CHECK (noteOf (e4).velocity == 101);
        phazite::test::dispatchPendingMessages();
    }

    // Typed, every note's velocity and length become it; a pitch typed moves the lowest there, the other with it.
    type (findField (screen, "Velocity"), "90");
    CHECK (noteOf (c4).velocity == 90);
    CHECK (noteOf (e4).velocity == 90);
    type (findField (screen, "Length"), "1/16");
    CHECK (noteOf (c4).length.inBeats() == doctest::Approx (0.25));
    CHECK (noteOf (e4).length.inBeats() == doctest::Approx (0.25));
    type (findField (screen, "Pitch"), "D4");
    CHECK (noteOf (c4).pitch == 62);
    CHECK (noteOf (e4).pitch == 66);

    // Starts by a step of a sixteenth, together; and typed as a bar and a beat, the earliest there.
    step (findField (screen, "Starts"), true);
    CHECK (noteOf (c4).start.inBeats() == doctest::Approx (0.25));
    CHECK (noteOf (e4).start.inBeats() == doctest::Approx (1.25));
    type (findField (screen, "Starts"), "1.3");
    CHECK (noteOf (c4).start.inBeats() == doctest::Approx (2.0));
    CHECK (noteOf (e4).start.inBeats() == doctest::Approx (3.0));

    // Transposed together only as far as both can go: the highest stops at 127.
    phazite::changeNotes (screen.context, { c4, e4 }, phazite::NoteProperty::pitch, 100.0);
    CHECK (noteOf (e4).pitch == 127);
    CHECK (noteOf (c4).pitch == 123);
}

TEST_CASE ("Selected clips' starts, lengths, gain and pitch change together, and selected automation points' too")
{
    Screen screen;
    auto& selection = screen.context.getSongSelection();
    const auto first = phazite::addMidiClipAt (screen.context, screen.track (0), BeatPosition());
    const auto second = phazite::addMidiClipAt (screen.context, screen.track (1), BeatPosition::fromBeats (8.0));
    selection.selectOnly (first);
    selection.add (second);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Clips"));
    CHECK (screen.shows ("Bar 1 to Bar 3"));
    CHECK_FALSE (findField (screen, "Gain").isVisible()); // MIDI clips have none

    const auto clipOf = [&screen] (phazite::ClipID id) { return *phazite::getClip (screen.context, id); };
    step (findField (screen, "Starts"), true);
    CHECK (clipOf (first).range.getStart().inBeats() == doctest::Approx (1.0));
    CHECK (clipOf (second).range.getStart().inBeats() == doctest::Approx (9.0));
    type (findField (screen, "Length"), "2 bars");
    CHECK (clipOf (first).range.getLength().inBeats() == doctest::Approx (8.0));
    CHECK (clipOf (second).range.getLength().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Change Clip Length"));

    // Two points of the first track's volume, at 0 and -12 dB: their range, a step up for both, and a value typed for both.
    const auto volume = phazite::getVolume (screen.context, screen.track (0)).getRef();
    phazite::addAutomationPoint (screen.context, volume, { BeatPosition(), phazite::decibelsToFaderPosition (0.0f) });
    phazite::addAutomationPoint (screen.context, volume, { BeatPosition::fromBeats (4.0), phazite::decibelsToFaderPosition (-12.0f) });
    selection.selectAutomationPoints (volume, { 0, 1 });
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Automation"));
    CHECK (screen.shows ("2 points"));
    CHECK (screen.shows ("Bar 1 to Bar 2"));

    const auto valueOf = [&screen, &volume] (int index)
    { return phazite::getAutomationPoints (screen.context, volume)[(size_t) index].value; };
    const auto before = valueOf (1);
    step (findField (screen, "Value"), true);
    CHECK (valueOf (1) > before);
    CHECK (valueOf (0) - valueOf (1) == doctest::Approx (phazite::decibelsToFaderPosition (0.0f) - before).epsilon (0.001));
    type (findField (screen, "Value"), "-6 dB");
    CHECK (valueOf (0) == doctest::Approx (phazite::decibelsToFaderPosition (-6.0f)));
    CHECK (valueOf (1) == doctest::Approx (phazite::decibelsToFaderPosition (-6.0f)));
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Set Automation Value"));
}

TEST_CASE ("The inspector fills its panel, and scrolls up and down when the panel is shorter than what it shows")
{
    Screen screen;
    screen.context.getSongSelection().selectOnly (screen.track (0));
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Track"));

    auto* viewport = phazite::test::findAll<juce::Viewport> (screen.inspector).getFirst();
    REQUIRE (viewport != nullptr);
    auto* page = viewport->getViewedComponent();
    REQUIRE (page != nullptr);
    CHECK (page->getWidth() >= 300 - viewport->getScrollBarThickness());

    // Under its title, which stays at the top as the rows scroll under it, beside the panel's buttons.
    CHECK (viewport->getY() > 0);
    CHECK (viewport->getBottom() == screen.inspector.getHeight());
    CHECK (page->getHeight() >= viewport->getHeight());
    juce::Label* title = nullptr;

    for (auto* child : screen.inspector.getChildren())
        if (auto* label = dynamic_cast<juce::Label*> (child))
            title = label;

    REQUIRE (title != nullptr);
    CHECK (title->getText().isNotEmpty()); // the track's name
    CHECK (title->getBottom() <= viewport->getY());

    // In its title's row, short of the panel's X at its right end, which the shell puts there.
    const auto row = screen.inspector.getTitleRow();
    CHECK (row.getWidth() == screen.inspector.getWidth());
    CHECK (row.contains (title->getBounds()));
    screen.inspector.setTitleRoom (40);
    CHECK (title->getRight() == screen.inspector.getWidth() - 40);
    screen.inspector.setTitleRoom (0);
    CHECK (title->getRight() == screen.inspector.getWidth() - phazite::ui::Space::m);

    screen.window.setSize (300, 200);
    CHECK (page->getHeight() > 200); // it scrolls
}

TEST_CASE ("Enter in a name field renames and gives the keys back to the window, as Escape does, the field unselected")
{
    Screen screen;
    screen.context.getSongSelection().selectOnly (screen.track (1));
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Track"));
    auto* name = dynamic_cast<juce::TextEditor*> (phazite::test::findComponentWithTitle (screen.inspector, "Name"));
    REQUIRE (name != nullptr);

    // Clicked, it has the keys and its text is selected; typed over and Enter, the track has the name, and the keys are the
    // window's again (Space plays), the field showing no selection.
    const auto click = [&screen, name]
    {
        phazite::test::Mouse mouse (*name);
        mouse.click (name->getLocalBounds().getCentre());
        name->selectAll();
        REQUIRE (name->hasKeyboardFocus (false));
    };

    click();

    for (const auto letter : { 'l', 'e', 'a', 'd' })
        screen.window.pressKey (juce::KeyPress (letter, 0, letter));

    screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getTrackRow (screen.context, screen.track (1))->name == juce::String ("lead"));
    CHECK (juce::Component::getCurrentlyFocusedComponent() == nullptr);
    CHECK (name->getHighlightedRegion().isEmpty());

    // Escape too, keeping what was typed, as leaving the field does.
    click();
    screen.window.pressKey (juce::KeyPress ('x', 0, 'x'));
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    phazite::test::dispatchPendingMessages();
    CHECK (juce::Component::getCurrentlyFocusedComponent() == nullptr);
    CHECK (name->getHighlightedRegion().isEmpty());
    CHECK (phazite::getTrackRow (screen.context, screen.track (1))->name == juce::String ("x"));
}
