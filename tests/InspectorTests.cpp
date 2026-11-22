// SPDX-License-Identifier: AGPL-3.0-or-later
// The inspector (src/ui/mixer/Inspector.h; roadmap step 2.5), driven as the user would: the properties of what is
// selected (a track, clips, notes, automation points, a section), and the song's with nothing selected; changed from its
// rows, each one undo step, several items at once.
#include "AppScreen.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TempoModel.h"
#include "model/transport/TransportModel.h"
#include "ui/components/Controls.h"
#include "ui/components/ViewTitle.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/Inspector.h"
#include "ui/theme/DesignTokens.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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

    // A label's, a button's or the title's text anywhere in the inspector.
    bool shows (const juce::String& text)
    {
        const auto labels = phazite::test::findAll<juce::Label> (inspector);
        const auto buttons = phazite::test::findAll<juce::TextButton> (inspector);
        const auto titles = phazite::test::findAll<phazite::ui::ViewTitle> (inspector);
        return std::any_of (labels.begin(), labels.end(), [&text] (juce::Label* label) { return label->getText() == text; })
               || std::any_of (buttons.begin(), buttons.end(),
                               [&text] (juce::TextButton* button) { return button->getButtonText() == text; })
               || std::any_of (titles.begin(), titles.end(), [&text] (phazite::ui::ViewTitle* title) { return title->getName() == text; });
    }

    phazite::Song::Ptr song = phazite::createSong(); // 120 BPM, 4/4
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Inspector inspector { commands, context };
    phazite::test::TestWindow window { inspector, 300, 900 };
};

// Every component in a view, each held so as to tell whether it is still there.
std::vector<juce::Component::SafePointer<juce::Component>> holdEverything (juce::Component& view)
{
    std::vector<juce::Component::SafePointer<juce::Component>> held;

    for (auto* each : phazite::test::findAll<juce::Component> (view))
        held.emplace_back (each);

    return held;
}

// How many components in a view weren't there when they were held: made since (one made where another went counts too).
int countMade (const std::vector<juce::Component::SafePointer<juce::Component>>& held, juce::Component& view)
{
    auto made = 0;

    for (auto* each : phazite::test::findAll<juce::Component> (view))
        if (std::none_of (held.begin(), held.end(), [each] (const auto& one) { return one.getComponent() == each; }))
            ++made;

    return made;
}
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

    // While the tempo changes along the song, how far it goes.
    phazite::addTempoChange (screen.context, phazite::BeatPosition::fromBeats (16.0), 90.0);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.shows ("90 to 120 BPM"));
    phazite::removeTempoChanges (screen.context);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.shows ("120 BPM"));

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

TEST_CASE ("Typing the lowest of several items' values sets every item to it, as any value typed does")
{
    // The field's value is the lowest item's, and JUCE's slider skips a value typed equal to its own: with velocities 64 and
    // 100, typing 64 did nothing (known bug 57).
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
    REQUIRE (screen.shows ("64 to 100"));
    const auto noteOf = [&screen] (phazite::NoteRef ref) { return *phazite::getNote (screen.context, ref); };
    phazite::test::UndoCheck undo (*screen.song);

    undo.check ("the lowest velocity typed", [&] { type (findField (screen, "Velocity"), "64"); });
    CHECK (noteOf (c4).velocity == 64);
    CHECK (noteOf (e4).velocity == 64);
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Change Velocity"));

    undo.check ("the shortest length typed", [&] { type (findField (screen, "Length"), "1/8"); });
    CHECK (noteOf (c4).length.inBeats() == doctest::Approx (0.5));

    // And a step after it is a step, not a value typed again.
    undo.check ("a step up", [&] { step (findField (screen, "Velocity"), true); });
    CHECK (noteOf (c4).velocity == 65);
    CHECK (noteOf (e4).velocity == 65);

    // A value typed that differs from the field's in its last bits only, which JUCE's slider takes for the same (it compares
    // them approximately): set, and not left over for the next step, which then set it again instead of stepping (a reviewer,
    // 2026-11-14).
    undo.check ("the same velocity typed, but for its last bits", [&] { type (findField (screen, "Velocity"), "65.00000000000001"); });
    undo.check ("a step up after it", [&] { step (findField (screen, "Velocity"), true); });
    CHECK (noteOf (c4).velocity == 66);
    CHECK (noteOf (e4).velocity == 66);
    undo.walkBackAndForth();
}

TEST_CASE ("Selected notes' MIDI channel shows as a range, moves by a step for each within 1 to 16, and typed becomes each one's")
{
    // A new note is on channel 1; the inspector's Channel field sets the selected notes' channel, as the other fields do.
    Screen screen;
    using phazite::BeatDuration;
    const auto clip = phazite::addMidiClipAt (screen.context, screen.track (0), BeatPosition());
    const auto c4 = phazite::addNote (screen.context, clip, 60, BeatPosition(), BeatDuration::fromBeats (1.0), 64);
    const auto e4 = phazite::addNote (screen.context, clip, 64, BeatPosition::fromBeats (1.0), BeatDuration::fromBeats (0.5), 100);
    screen.song->getUndoManager().clearUndoHistory();
    auto& selection = screen.context.getSongSelection();
    selection.setNoteClips ({ clip });
    selection.selectNotes ({ c4 });
    phazite::test::dispatchPendingMessages();
    const auto noteOf = [&screen] (phazite::NoteRef ref) { return *phazite::getNote (screen.context, ref); };
    REQUIRE (noteOf (c4).channel == 1);
    phazite::test::UndoCheck undo (*screen.song);

    // One note: typed, its channel; the song keeps it.
    undo.check ("channel 10 typed", [&] { type (findField (screen, "Channel"), "10"); });
    CHECK (noteOf (c4).channel == 10);
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Change Channel"));
    CHECK (phazite::getNoteData (*screen.song, c4)->channel == 10);

    // Both, on channels 10 and 1: shown as a range; a step up moves each, one at most to 16.
    selection.selectNotes ({ c4, e4 });
    phazite::test::dispatchPendingMessages();
    CHECK (screen.shows ("1 to 10"));
    undo.check ("a step up", [&] { step (findField (screen, "Channel"), true); });
    CHECK (noteOf (c4).channel == 11);
    CHECK (noteOf (e4).channel == 2);
    phazite::changeNotes (screen.context, { c4, e4 }, phazite::NoteProperty::channel, 10.0);
    CHECK (noteOf (c4).channel == 16);
    CHECK (noteOf (e4).channel == 12);

    // Typed, each becomes it; back to 1, the song writes none.
    undo.check ("channel 1 typed", [&] { type (findField (screen, "Channel"), "1"); });
    CHECK (noteOf (c4).channel == 1);
    CHECK (noteOf (e4).channel == 1);
    const auto saved =
        phazite::copyWholeClip (*screen.song, c4.clip).getChildWithProperty (phazite::schema::noteID, (juce::int64) c4.note.getRaw());
    REQUIRE (saved.isValid());
    CHECK_FALSE (saved.hasProperty (phazite::schema::noteChannel));
    undo.walkBackAndForth();
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
    { return phazite::getAutomationPoints (screen.context, volume)[(size_t) index].position; };
    const auto before = valueOf (1);
    step (findField (screen, "Value"), true);
    CHECK (valueOf (1) > before);
    CHECK (valueOf (0) - valueOf (1) == doctest::Approx (phazite::decibelsToFaderPosition (0.0f) - before).epsilon (0.001));
    type (findField (screen, "Value"), "-6 dB");
    CHECK (valueOf (0) == doctest::Approx (phazite::decibelsToFaderPosition (-6.0f)));
    CHECK (valueOf (1) == doctest::Approx (phazite::decibelsToFaderPosition (-6.0f)));
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Set Automation Value"));
}

TEST_CASE ("A value typed that is no finite number changes nothing: a clip's start and length, a point's value and place, a note's")
{
    // A reviewer, of known bug 148's fix: "inf/inf" read as a length of no number, "1e999/1" as an infinite one, and a beat of
    // four hundred digits as an infinite place, which reached the model, which passed them on. Refused by the fields, and
    // by the model's setters, which don't count on the view.
    Screen screen;
    auto& selection = screen.context.getSongSelection();
    const auto clip = phazite::addMidiClipAt (screen.context, screen.track (0), BeatPosition::fromBeats (4.0));
    const auto note =
        phazite::addNote (screen.context, clip, 60, BeatPosition::fromBeats (4.0), phazite::BeatDuration::fromBeats (1.0), 64);
    const auto clipOf = [&screen, clip] { return *phazite::getClip (screen.context, clip); };
    const auto range = clipOf().range;
    const auto manyDigits = "1 " + juce::String::repeatedString ("9", 400);

    selection.selectOnly (clip);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Clip"));

    for (const auto& text :
         { juce::String ("inf/inf"), juce::String ("1e999/1"), juce::String ("1/0"), juce::String::repeatedString ("9", 400) })
    {
        CAPTURE (text);
        type (findField (screen, "Length"), text);
        CHECK (clipOf().range == range);
    }

    type (findField (screen, "Starts"), manyDigits); // its bar, then a beat of four hundred digits
    CHECK (clipOf().range == range);

    for (const auto value : { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
    {
        for (const auto property : { phazite::ClipProperty::start, phazite::ClipProperty::length })
        {
            phazite::setClipsTo (screen.context, { clip }, property, value);
            phazite::changeClips (screen.context, { clip }, property, value);
        }

        CHECK (clipOf().range == range);
    }

    // A point's value and place.
    const auto volume = phazite::getVolume (screen.context, screen.track (0)).getRef();
    phazite::addAutomationPoint (screen.context, volume, { BeatPosition::fromBeats (4.0), 0.5f });
    const auto point = [&screen, &volume] { return phazite::getAutomationPoints (screen.context, volume).front(); };
    selection.selectAutomationPoints (volume, { 0 });
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Automation"));
    type (findField (screen, "Position"), manyDigits);
    type (findField (screen, "Value"), "1e999");

    for (const auto value : { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
    {
        phazite::setAutomationPointPositions (screen.context, volume, { 0 }, value);
        phazite::setAutomationCurves (screen.context, volume, { 0 }, value);
        phazite::moveAutomationPoints (screen.context, volume, { 0 }, phazite::BeatDuration::fromBeats ((double) value), value);
    }

    CHECK (point().beat.inBeats() == doctest::Approx (4.0));
    CHECK (point().position == doctest::Approx (0.5f));
    CHECK (std::isfinite (point().curve));

    // A note's, typed.
    selection.setNoteClips ({ clip });
    selection.selectNotes ({ note });
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Note"));
    type (findField (screen, "Velocity"), "1e999");
    type (findField (screen, "Length"), "inf/inf");
    type (findField (screen, "Starts"), manyDigits);
    const auto after = *phazite::getNote (screen.context, note);
    CHECK (after.velocity == 64);
    CHECK (after.length.inBeats() == doctest::Approx (1.0));
    CHECK (after.start.inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("In 6/8 a clip's place and length read in bars and eighths, and are typed so")
{
    Screen screen;
    phazite::setTimeSignature (screen.context, { 6, 8 });

    // A bar-long MIDI clip at the second bar: three quarter notes in, three long.
    const auto clip = phazite::addMidiClipAt (screen.context, screen.track (0), BeatPosition::fromBeats (3.0));
    REQUIRE (clip.isValid());
    const auto rangeOf = [&screen, clip] { return phazite::getClip (screen.context, clip)->range; };
    CHECK (rangeOf().getLength().inBeats() == doctest::Approx (3.0));
    screen.context.getSongSelection().selectOnly (clip);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Clip"));
    CHECK (screen.shows ("Bar 2"));
    CHECK (screen.shows ("1 bar"));

    // Bar 2, beat 4: its fourth eighth, a quarter note and a half into it.
    type (findField (screen, "Starts"), "2.4");
    CHECK (rangeOf().getStart().inBeats() == doctest::Approx (4.5));
    CHECK (screen.shows ("Bar 2, beat 4"));

    // A bar and two eighths: four quarter notes; a quarter note, as a note value: two eighths; three beats: three eighths.
    type (findField (screen, "Length"), "1 bar, 2 beats");
    CHECK (rangeOf().getLength().inBeats() == doctest::Approx (4.0));
    CHECK (screen.shows ("1 bar, 2 beats"));
    type (findField (screen, "Length"), "1/4");
    CHECK (rangeOf().getLength().inBeats() == doctest::Approx (1.0));
    CHECK (screen.shows ("2 beats"));
    type (findField (screen, "Length"), "3");
    CHECK (rangeOf().getLength().inBeats() == doctest::Approx (1.5));
    CHECK (screen.shows ("3 beats"));

    // Stepped by an eighth, the time signature's beat, as their text counts: to bar 2, beat 5, and an eighth shorter.
    step (findField (screen, "Starts"), true);
    CHECK (rangeOf().getStart().inBeats() == doctest::Approx (5.0));
    CHECK (screen.shows ("Bar 2, beat 5"));
    step (findField (screen, "Length"), false);
    CHECK (rangeOf().getLength().inBeats() == doctest::Approx (1.0));
    CHECK (screen.shows ("2 beats"));
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
    phazite::ui::ViewTitle* title = nullptr;

    for (auto* child : screen.inspector.getChildren())
        if (auto* viewTitle = dynamic_cast<phazite::ui::ViewTitle*> (child))
            title = viewTitle;

    REQUIRE (title != nullptr);
    CHECK (title->getName().isNotEmpty()); // the track's name
    CHECK (title->getBottom() <= viewport->getY());

    // In its title's row, short of the panel's X at its right end, which the shell puts there.
    const auto row = screen.inspector.getTitleRow();
    CHECK (row.getWidth() == screen.inspector.getWidth());
    CHECK (row.contains (title->getBounds()));
    screen.inspector.setTitleRoom (40);
    CHECK (title->getRight() == screen.inspector.getWidth() - 40);
    screen.inspector.setTitleRoom (0);
    CHECK (title->getRight() == screen.inspector.getWidth() - phazite::ui::SideBar::titlePadding);

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
    const auto click = [name]
    {
        phazite::test::Mouse mouse (*name);
        mouse.click (name->getLocalBounds().getCentre());
        name->selectAll();
        REQUIRE (name->hasKeyboardFocus (false));
    };

    click();

    for (const auto letter : { 'l', 'e', 'a', 'd' })
        screen.window.pressKey (juce::KeyPress (letter, 0, (juce::juce_wchar) letter));

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

TEST_CASE ("The inspector makes nothing while its panel is hidden, shows what is selected as it shows again, and a page takes more "
           "of its kind")
{
    // The threading audit, 2026-11-20: it made a whole page anew at each change of what was selected (a rubber band's at
    // each turn of the drag), with its panel closed too.
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("track.add"));
    const auto track = phazite::getAllPlaylistTracks (context).back();
    std::vector<phazite::ClipID> clips;

    for (int i = 0; i < 10; ++i)
    {
        clips.push_back (phazite::addMidiClipAt (context, track, BeatPosition::fromBeats (4.0 * i)));
        REQUIRE (clips.back().isValid());
    }

    auto* inspector = phazite::test::findAll<phazite::ui::Inspector> (screen.shell).getFirst();
    REQUIRE (inspector != nullptr);
    auto& selection = context.getSongSelection();

    // Selected as a rubber band selects them, a clip more at each turn of the drag.
    const auto selectFirst = [&] (size_t count)
    {
        selection.clear();

        for (size_t i = 0; i < count; ++i)
            selection.add (clips[i]);

        phazite::test::dispatchPendingMessages();
    };

    const auto titled = [inspector] (const juce::String& text)
    {
        const auto titles = phazite::test::findAll<phazite::ui::ViewTitle> (*inspector);
        return std::any_of (titles.begin(), titles.end(), [&text] (phazite::ui::ViewTitle* title) { return title->getName() == text; });
    };

    // Its panel hidden, a hundred turns of the drag make nothing in it.
    if (! inspector->isShowing())
        REQUIRE (screen.registry.invoke ("view.toggleInspector"));

    selectFirst (1);
    REQUIRE (inspector->getShown() == juce::String ("Clip"));
    REQUIRE (screen.registry.invoke ("view.toggleInspector"));
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (inspector->isShowing());
    const auto before = holdEverything (*inspector);

    for (int turn = 0; turn < 100; ++turn)
        selectFirst ((size_t) (turn % 10) + 1);

    CHECK (countMade (before, *inspector) == 0);

    // Shown again, it shows the clips selected last, as it would have.
    REQUIRE (screen.registry.invoke ("view.toggleInspector"));
    phazite::test::dispatchPendingMessages();
    REQUIRE (inspector->isShowing());
    CHECK (inspector->getShown() == juce::String ("Clips"));
    CHECK (titled ("10 clips"));

    // While it shows, the page of clips takes other clips rather than being made anew (as notes' page takes other notes,
    // and automation's other points of its parameter).
    selectFirst (1);
    REQUIRE (inspector->getShown() == juce::String ("Clip"));
    const auto shown = holdEverything (*inspector);
    selectFirst (3);
    CHECK (inspector->getShown() == juce::String ("Clips"));
    CHECK (titled ("3 clips"));
    CHECK (countMade (shown, *inspector) == 0);
}

TEST_CASE ("While a field of the inspector's page has the keys, other items of its kind selected make a new page")
{
    // The review of the threading audit's fix, 2026-11-20: a page took the new selection while its name field had the keys,
    // so the name typed for one clip, with Enter, would have renamed the clips selected after.
    Screen screen;
    auto& selection = screen.context.getSongSelection();
    std::vector<phazite::ClipID> clips;

    for (int i = 0; i < 3; ++i)
    {
        clips.push_back (phazite::addMidiClipAt (screen.context, screen.track (0), BeatPosition::fromBeats (4.0 * i)));
        REQUIRE (clips.back().isValid());
    }

    selection.selectOnly (clips[0]);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Clip"));
    auto* name = dynamic_cast<juce::TextEditor*> (phazite::test::findComponentWithTitle (screen.inspector, "Name"));
    REQUIRE (name != nullptr);
    phazite::test::Mouse mouse (*name);
    mouse.click (name->getLocalBounds().getCentre());
    REQUIRE (name->hasKeyboardFocus (false));
    const juce::Component::SafePointer<juce::Component> typedIn (name);

    for (const auto clip : { clips[1], clips[2] })
        selection.add (clip);

    phazite::test::dispatchPendingMessages();
    CHECK (screen.inspector.getShown() == juce::String ("Clips"));
    CHECK (typedIn == nullptr); // made anew, the field with it

    // Without the keys in it, the page of clips takes other clips.
    const auto shown = holdEverything (screen.inspector);
    selection.selectOnly (clips[1]);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.inspector.getShown() == juce::String ("Clip"));
    CHECK (countMade (shown, screen.inspector) == 0);
}
