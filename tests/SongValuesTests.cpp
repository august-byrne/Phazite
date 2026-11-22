// SPDX-License-Identifier: AGPL-3.0-or-later
// A MIDI clip's notes and controllers' values as a value (model/base/internal/SongValues.h: ClipNotes), which the song holds
// beside its tree: the conversion between the value and a clip's element (the one place that reads and writes NOTE and
// CONTROL by name), and the keeper, which keeps the song's values and its tree together for every change of the tree that
// isn't the notes' own: a clip that comes into the song with NOTE and CONTROL elements has them read into its value and taken
// out of the tree; a clip taken out has its value remembered and given back if the same element comes back (a move); a clip
// that joins an alias group takes the group's one value. Each rule through the tree and through the model's functions, with
// its Undo and Redo (UndoCheck.h), the song's values checked after each (SongCheck.h: values).
//   phazite_tests.exe --test-case="Song values*"
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/AudioRecorder.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::ClipID;
using phazite::ClipNotes;
using phazite::ControlKind;
using phazite::ControlValues;
using phazite::NoteID;
using phazite::NoteValues;

// Each change a tree's listeners hear, under its root.
struct Heard final : juce::ValueTree::Listener
{
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++properties; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++added; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++removed; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { ++moved; }

    int count() const { return properties + added + removed + moved; }

    int properties = 0, added = 0, removed = 0, moved = 0;
};

// What the song's values tell of the clips whose values changed.
struct Told final : phazite::SongValues::NotesListener
{
    void clipNotesChanged (const std::vector<ClipID>& clips) override { told.push_back (clips); }

    std::vector<std::vector<ClipID>> told;
};

NoteValues makeNote (std::uint64_t id, int pitch, double start)
{
    return { .id = NoteID::fromRaw (id), .pitch = pitch, .start = phazite::BeatPosition::fromBeats (start) };
}

ControlValues makeControl (double beat, int value)
{
    return { .kind = ControlKind::controller, .number = 64, .beat = beat, .value = value };
}

// A clip's element with three notes and three controllers' values, interleaved as an older build's recording left them.
juce::ValueTree makeClipElement()
{
    juce::ValueTree clip (schema::CLIP);
    clip.setProperty (schema::clipKind, schema::clipIsMidi, nullptr);
    const auto value = phazite::makeClipNotes ({ makeNote (1, 60, 0.0), makeNote (2, 62, 1.0), makeNote (3, 64, 2.0) },
                                               { makeControl (0.0, 127), makeControl (0.5, 0), makeControl (1.5, 127) });
    juce::ValueTree written (schema::CLIP);
    phazite::writeClipNotes (value, written);

    for (const auto index : { 0, 3, 1, 4, 2, 5 })
        clip.appendChild (written.getChild (index).createCopy(), nullptr);

    return clip;
}

std::vector<juce::String> getChildTypes (const juce::ValueTree& clip)
{
    std::vector<juce::String> types;

    for (const auto& child : clip)
        types.push_back (child.getType().toString()
                         + (child.hasProperty (schema::noteID) ? child[schema::noteID].toString() : juce::String()));

    return types;
}

bool holdsEvents (const juce::ValueTree& clip)
{
    for (const auto& child : clip)
        if (child.hasType (schema::NOTE) || child.hasType (schema::CONTROL))
            return true;

    return false;
}

// The clips of each golden project that hold notes or values, each whole (WholeState.h), with its project open.
struct GoldenClips
{
    phazite::test::TemporaryFolder temporary;
    std::vector<std::unique_ptr<phazite::Project>> projects;
    std::vector<juce::ValueTree> clips;

    GoldenClips()
    {
        for (const auto* name : { "projects/representative.xml", "commands/script.xml" })
        {
            const auto file = temporary.folder.getChildFile (juce::String (projects.size()) + ".phazite");
            REQUIRE (phazite::test::getGoldenFolder().getChildFile (name).copyFileTo (file));
            juce::String error;
            auto project = phazite::Project::open (file, false, error);
            REQUIRE_MESSAGE (project != nullptr, error.toStdString());

            for (const auto& clip : phazite::getClipPlacements (project->getSong()))
            {
                CHECK_FALSE (holdsEvents (phazite::findClipState (project->getSong(), clip.id))); // the song's own tree holds none

                if (const auto whole = phazite::copyWholeClip (project->getSong(), clip.id); ! clip.isAudio() && whole.getNumChildren() > 0)
                    clips.push_back (whole);
            }

            projects.push_back (std::move (project));
        }
    }
};

// A song of three tracks, the first with a clip of notes and values, with every change checked for an exact undo.
struct Kept
{
    Kept()
    {
        tracks = phazite::ensureTracks (*song, 3);
        clip = phazite::insertMidiClip (*song, tracks[0], { phazite::test::at (0.0), phazite::test::at (8.0) }, nullptr);
        REQUIRE (clip.isValid());
        phazite::addNote (*song, clip, 60, phazite::test::at (1.0), phazite::test::lasting (1.0), 100);
        phazite::addNote (*song, clip, 64, phazite::test::at (2.0), phazite::test::lasting (1.0), 90);
        phazite::addControl (*song, clip, phazite::test::at (0.5), ControlKind::controller, 64, 127);
        song->getUndoManager().clearUndoHistory();
        song->getValues()->addNotesListener (&told);
    }

    ~Kept() { song->getValues()->removeNotesListener (&told); }

    const ClipNotes* find (ClipID id) const { return song->getValues()->findClipNotes (id); }
    ClipNotes get (ClipID id) const { return phazite::getClipNotesValue (*song, id); }
    juce::ValueTree element (ClipID id) const { return phazite::findClipState (*song, id); }

    // A step of the test's own, named, checked exact, and the song's values with its tree after it, its Undo and its Redo.
    void step (const juce::String& name, const std::function<void()>& change)
    {
        check.check (name,
                     [&]
                     {
                         const phazite::UndoStep undoStep (*song, name);
                         change();
                     });

        phazite::test::checkValues (*song);
        phazite::undo (*song);
        phazite::test::checkValues (*song);
        phazite::redo (*song);
        phazite::test::checkValues (*song);
    }

    phazite::Song::Ptr song = phazite::createSong();
    std::vector<phazite::TrackID> tracks;
    ClipID clip;
    Told told;
    phazite::test::UndoCheck check { *song };
};
} // namespace

//==============================================================================
TEST_CASE ("Song values: every clip of the golden projects, read into a value and written into an emptied copy, holds its notes "
           "first, then its values, each kind in its order")
{
    GoldenClips golden;
    REQUIRE (golden.clips.size() >= 4);

    for (const auto& clip : golden.clips)
    {
        const auto value = phazite::readClipNotes (clip);
        CHECK (value.holdsItsPlaces());
        CHECK ((int) (value.notes.size() + value.controls.size()) == clip.getNumChildren());

        // Emptied, then given its value: its notes, then its values, each kind in its order, as whole state writes them.
        auto written = clip.createCopy();
        written.removeAllChildren (nullptr);
        phazite::writeClipNotes (value, written);
        CHECK (phazite::readClipNotes (written) == value);
        CHECK_MESSAGE (written.isEquivalentTo (clip), clip.toXmlString().toStdString());
    }
}

TEST_CASE ("Song values: a clip's notes, in any order among its values, read into a value and written back are the same notes "
           "and values")
{
    const auto clip = makeClipElement();
    REQUIRE (getChildTypes (clip) == std::vector<juce::String> { "NOTE1", "CONTROL", "NOTE2", "CONTROL", "NOTE3", "CONTROL" });
    const auto value = phazite::readClipNotes (clip);
    CHECK (value.notes.size() == 3);
    CHECK (value.controls.size() == 3);

    juce::ValueTree written (schema::CLIP);
    phazite::writeClipNotes (value, written);
    CHECK (getChildTypes (written) == std::vector<juce::String> { "NOTE1", "NOTE2", "NOTE3", "CONTROL", "CONTROL", "CONTROL" });
    CHECK (phazite::readClipNotes (written) == value);
}

TEST_CASE ("Song values: a clip's notes made by the model, every value a note can have, written whole and read back")
{
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    const auto clip = phazite::insertMidiClip (*song, track, { phazite::test::at (0.0), phazite::test::at (64.0) }, nullptr);
    std::vector<phazite::NoteData> made;

    for (int i = 0; i < 200; ++i)
    {
        phazite::NoteData note { .clip = clip,
                                 .pitch = 40 + i % 40,
                                 .start = phazite::test::at (i * 0.25),
                                 .length = phazite::test::lasting (0.2),
                                 .velocity = 1 + i % 127 };
        note.muted = i % 7 == 0;
        note.slide = i % 11 == 0;
        note.portamento = i % 13 == 0;
        note.channel = 1 + i % 16;
        made.push_back (note);
    }

    phazite::setClipNotes (*song, clip, made);
    CHECK_FALSE (holdsEvents (phazite::findClipState (*song, clip)));
    const auto whole = phazite::copyWholeClip (*song, clip);
    const auto value = phazite::getClipNotesValue (*song, clip);
    REQUIRE (value.notes.size() == 200);
    CHECK (whole.getNumChildren() == 200);
    CHECK (value == phazite::readClipNotes (whole));
    CHECK (phazite::readClipNotes (whole).isSameAs (value) == false); // read anew: alike, not the same

    // Kept with a copy of the tree, then written into it: the same.
    const auto kept = phazite::keepValues (*song);
    CHECK (phazite::copyWhole (song->getState().createCopy(), kept).isEquivalentTo (phazite::copyWholeSong (*song)));
    CHECK (phazite::holdSameNotes (kept, phazite::keepValues (*song), clip));
}

TEST_CASE ("Song values: the helpers that change a value in batches keep where each note is by its ID")
{
    std::vector<NoteValues> notes;

    for (std::uint64_t i = 1; i <= 100; ++i)
        notes.push_back (makeNote (i * 3, 40 + (int) (i % 40), (double) i * 0.25));

    auto value = phazite::makeClipNotes (notes, { makeControl (0.0, 1), makeControl (1.0, 2) });
    CHECK (value.holdsItsPlaces());
    REQUIRE (value.find (NoteID::fromRaw (30)) != nullptr);
    CHECK (value.find (NoteID::fromRaw (30))->pitch == 50);
    CHECK (value.find (NoteID::fromRaw (31)) == nullptr);

    value = phazite::withNotesChanged (value, { NoteID::fromRaw (30), NoteID::fromRaw (31), NoteID::fromRaw (300) },
                                       [] (NoteValues& note) { note.pitch = 0; });
    CHECK (value.holdsItsPlaces());
    CHECK (value.find (NoteID::fromRaw (30))->pitch == 0);
    CHECK (value.find (NoteID::fromRaw (300))->pitch == 0);
    CHECK (value.find (NoteID::fromRaw (27))->pitch != 0);

    value = phazite::withoutNotes (value, [] (const NoteValues& note) { return note.id.getRaw() % 2 == 0; });
    CHECK (value.holdsItsPlaces());
    CHECK (value.notes.size() == 50);
    CHECK (value.find (NoteID::fromRaw (30)) == nullptr);
    CHECK (value.find (NoteID::fromRaw (33)) != nullptr);

    value = phazite::withNoteAdded (value, makeNote (1000, 99, 0.0));
    CHECK (value.holdsItsPlaces());
    CHECK (value.notes.back().id == NoteID::fromRaw (1000));

    value =
        phazite::withEachChanged (value, [] (NoteValues& note) { note.velocity = 1; }, [] (ControlValues& control) { control.value = 7; });
    CHECK (value.holdsItsPlaces());
    CHECK (value.find (NoteID::fromRaw (1000))->velocity == 1);
    CHECK (value.controls[1].value == 7);

    value = phazite::withoutControls (value, [] (size_t place, const ControlValues&) { return place == 0; });
    value = phazite::withControlsAdded (value, { makeControl (2.0, 3) });
    CHECK (value.holdsItsPlaces());
    CHECK (value.controls.size() == 2);
    CHECK (juce::exactlyEqual (value.controls[1].beat, 2.0));

    // A value read from a clip's element keeps them too.
    juce::ValueTree clip (schema::CLIP);
    phazite::writeClipNotes (value, clip);
    CHECK (phazite::readClipNotes (clip).holdsItsPlaces());
    CHECK (phazite::readClipNotes (clip) == value);
}

TEST_CASE ("Song values: the same value, and an equal one")
{
    const auto value = phazite::makeClipNotes ({ makeNote (1, 60, 0.0), makeNote (2, 62, 1.0) }, { makeControl (0.0, 127) });
    const auto copy = value;
    CHECK (copy.isSameAs (value));
    CHECK (copy == value);

    // Made again: equal, but not the same without a look inside.
    const auto again = phazite::makeClipNotes ({ makeNote (1, 60, 0.0), makeNote (2, 62, 1.0) }, { makeControl (0.0, 127) });
    CHECK_FALSE (again.isSameAs (value));
    CHECK (again == value);

    // A note changed, or a value: neither.
    const auto changed = phazite::withNotesChanged (value, { NoteID::fromRaw (2) }, [] (NoteValues& note) { note.velocity = 1; });
    CHECK_FALSE (changed.isSameAs (value));
    CHECK_FALSE (changed == value);
    const auto otherBeat = phazite::withEachChanged (value, [] (NoteValues&) {}, [] (ControlValues& control) { control.beat = 0.001; });
    CHECK_FALSE (otherBeat == value);

    // A change of nothing gives the same value back.
    CHECK (phazite::withoutNotes (value, [] (const NoteValues&) { return false; }).isSameAs (value));
    CHECK (phazite::withControlsAdded (value, {}).isSameAs (value));
    CHECK (ClipNotes().isSameAs (ClipNotes()));
}

TEST_CASE ("Song values: a property a note's element hasn't reads as what its description gives for a value that is no number")
{
    // A value always holds each: a pitch of 60, a start at the clip's own, a length of a beat, a velocity of 100
    // (model/base/internal/Schema.h), where the element read as 0 while the tree held the notes.
    juce::ValueTree note (schema::NOTE);
    note.setProperty (schema::noteID, 7, nullptr);
    const auto values = phazite::readNoteValues (note);
    CHECK (values.id == NoteID::fromRaw (7));
    CHECK (values.pitch == 60);
    CHECK (values.start == phazite::BeatPosition());
    CHECK (values.length == phazite::BeatDuration::fromBeats (1.0));
    CHECK (values.velocity == 100);

    // And one outside its range within it.
    note.setProperty (schema::noteVelocity, 0, nullptr);
    note.setProperty (schema::notePitch, 200, nullptr);
    CHECK (phazite::readNoteValues (note).velocity == 1);
    CHECK (phazite::readNoteValues (note).pitch == 127);
    note.removeProperty (schema::noteVelocity, nullptr);
    note.removeProperty (schema::notePitch, nullptr);
    CHECK_FALSE (values.muted);
    CHECK (values.channel == 1);

    note.setProperty (schema::noteChannel, 40, nullptr);
    CHECK (phazite::readNoteValues (note).channel == 16);

    // And a value a merge chose for one, as the element would read it.
    auto chosen = values;
    CHECK (phazite::setNoteProperty (chosen, schema::notePitch, 64));
    CHECK (phazite::setNoteProperty (chosen, schema::noteMute, true));
    CHECK (phazite::setNoteProperty (chosen, schema::noteChannel, juce::var()));
    CHECK (phazite::setNoteProperty (chosen, schema::noteVelocity, juce::var()));
    CHECK (chosen.velocity == 100);
    CHECK (chosen.pitch == 64);
    CHECK (chosen.muted);
    CHECK (chosen.channel == 1);
    CHECK_FALSE (phazite::setNoteProperty (chosen, schema::clipName, "x"));
}

//==============================================================================
TEST_CASE ("Song values: a song opened holds its clips' notes beside its tree, the clips of an alias group one value, and a note "
           "edit changes nothing in the tree")
{
    phazite::test::TemporaryFolder temporary;
    const auto file = temporary.folder.getChildFile ("Representative.phazite");
    REQUIRE (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml").copyFileTo (file));
    juce::String error;
    const auto project = phazite::Project::open (file, false, error);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    auto& song = project->getSong();
    phazite::test::checkValues (song);

    // The representative project's drum clip (8), its first note (9).
    const phazite::NoteRef note { ClipID::fromRaw (8), NoteID::fromRaw (9) };
    REQUIRE (phazite::getNoteData (song, note).has_value());
    CHECK (phazite::getNoteData (song, note)->pitch == 36);

    Heard heard;
    auto root = song.getState();
    root.addListener (&heard);

    {
        const phazite::UndoStep step (song, "Transpose Note");
        phazite::transposeNotes (song, { note }, 2);
    }

    root.removeListener (&heard);
    CHECK (heard.count() == 0);
    CHECK (phazite::getNoteData (song, note)->pitch == 38);
    CHECK (project->hasUnsavedChanges());

    // Written whole, the note holds what it did but its pitch.
    for (const auto& child : phazite::copyWholeClip (song, note.clip))
        if (child.hasType (schema::NOTE) && phazite::readNoteValues (child).id == note.note)
            CHECK ((int) child[schema::notePitch] == 38);

    phazite::undo (song);
    CHECK (phazite::getNoteData (song, note)->pitch == 36);
    CHECK_FALSE (project->checkUnsavedChanges()); // compared whole: as saved again
}

TEST_CASE ("Song values: a clip added with NOTE and CONTROL elements has them read into its value and taken out of the tree, in "
           "the step, and Undo and Redo take both back and forth")
{
    Kept k;
    auto element = juce::ValueTree (schema::CLIP);
    const auto id = ClipID::fromRaw (k.song->makeID());
    element.setProperty (schema::clipID, (juce::int64) id.getRaw(), nullptr);
    element.setProperty (schema::clipKind, schema::clipIsMidi, nullptr);
    element.setProperty (schema::clipStart, 16.0, nullptr);
    element.setProperty (schema::clipLength, 4.0, nullptr);
    element.appendChild (juce::ValueTree ("SOMETHING", { { "a", 1 } }), nullptr);

    for (const auto& child : makeClipElement())
        element.appendChild (child.createCopy(), nullptr);

    const auto expected = phazite::readClipNotes (element);

    // Through the tree, as a paste or a version puts a clip in.
    k.step ("Add Clip", [&] { k.element (k.clip).getParent().appendChild (element, &k.song->getUndoManager()); });
    REQUIRE (k.find (id) != nullptr);
    CHECK (*k.find (id) == expected);
    CHECK_FALSE (holdsEvents (k.element (id)));
    CHECK (k.element (id).getChildWithName ("SOMETHING").isValid()); // what isn't a note or a value stays
    CHECK (phazite::getNoteData (*k.song, { id, NoteID::fromRaw (2) })->pitch == 62);

    phazite::undo (*k.song);
    CHECK (k.find (id) == nullptr);
    phazite::redo (*k.song);
    CHECK (*k.find (id) == expected);

    // Through the model: a track duplicated holds its clips' values, each shared with its original until it is edited.
    std::vector<phazite::TrackID> copies;
    k.step ("Duplicate Track", [&] { copies = phazite::duplicateTracks (*k.song, { k.tracks[0] }, nullptr); });
    REQUIRE (copies.size() == 1);
    const auto copiedClips = phazite::getClips (*k.song, copies.front());
    REQUIRE (copiedClips.size() == 2);

    for (const auto copied : copiedClips)
        CHECK (k.find (copied)->isSameAs (
            *k.find (phazite::getClipRange (*k.song, copied).getStart() < phazite::test::at (8.0) ? k.clip : id)));

    k.check.walkBackAndForth();
}

TEST_CASE ("Song values: a clip moved between tracks, or with its track into a folder, keeps its value; one deleted has it back "
           "with Undo")
{
    Kept k;
    const auto value = k.get (k.clip);
    REQUIRE (value.notes.size() == 2);

    // Taken out and put in again, the same element.
    k.step ("Move Clip", [&] { phazite::moveClip (*k.song, k.clip, phazite::test::at (2.0), k.tracks[1]); });
    CHECK (phazite::getClipTrack (*k.song, k.clip) == k.tracks[1]);
    CHECK (k.find (k.clip)->isSameAs (value));

    phazite::TrackID folder;
    k.step ("Group Into Folder", [&] { folder = phazite::groupIntoFolder (*k.song, { k.tracks[1] }, nullptr); });
    REQUIRE (folder.isValid());
    CHECK (k.find (k.clip)->isSameAs (value));

    k.step ("Delete Clip", [&] { phazite::deleteClips (*k.song, { k.clip }, false); });
    CHECK (k.find (k.clip) == nullptr);
    CHECK_FALSE (phazite::hasNote (*k.song, { k.clip, value.notes[0].id }));

    phazite::undo (*k.song);
    CHECK (k.find (k.clip)->isSameAs (value));
    phazite::redo (*k.song);

    k.check.walkBackAndForth();
}

TEST_CASE ("Song values: a clip's copies share its value, an alias joins its group's one value, and one made unique keeps its "
           "own")
{
    Kept k;
    std::vector<ClipID> aliases, copies, halves;
    k.step ("Duplicate as Alias", [&] { aliases = phazite::copyClips (*k.song, { k.clip }, true); });
    REQUIRE (aliases.size() == 1);
    CHECK (k.find (aliases[0])->isSameAs (*k.find (k.clip)));

    k.step ("Duplicate", [&] { copies = phazite::copyClips (*k.song, { k.clip }, false); });
    REQUIRE (copies.size() == 1);
    CHECK (k.find (copies[0])->isSameAs (*k.find (k.clip))); // shared until either is edited

    // An edit through the alias reaches the clip, not the copy.
    k.step ("Add Note", [&] { phazite::addNote (*k.song, aliases[0], 72, phazite::test::at (4.0), phazite::test::lasting (1.0), 100); });
    CHECK (k.find (aliases[0])->isSameAs (*k.find (k.clip)));
    CHECK (k.find (k.clip)->notes.size() == 3);
    CHECK (k.find (copies[0])->notes.size() == 2);

    // A split's right half stays in the group, and shares its value.
    k.step ("Split", [&] { halves = phazite::splitClips (*k.song, { k.clip }, phazite::test::at (3.0)); });
    REQUIRE (halves.size() == 1);
    CHECK (k.find (halves[0])->isSameAs (*k.find (k.clip)));

    // A clip joined to a group (Clips.h: setClipLink(), which writes its link where the song keeps it) takes the group's value.
    k.step ("Link", [&] { phazite::setClipLink (*k.song, copies[0], phazite::getClipValue (*k.song, k.clip).value().link); });
    CHECK (k.find (copies[0])->isSameAs (*k.find (k.clip)));

    // Made unique, each keeps the value it held, and an edit of one is its own.
    k.step ("Make Unique", [&] { phazite::makeClipsUnique (*k.song, { aliases[0] }); });
    k.step ("Delete Note", [&] { phazite::deleteNotes (*k.song, { { aliases[0], k.find (aliases[0])->notes[0].id } }); });
    CHECK (k.find (aliases[0])->notes.size() == 2);
    CHECK (k.find (k.clip)->notes.size() == 3);

    k.check.walkBackAndForth();
}

TEST_CASE ("Song values: what is written outside the history is no undo step, and a clip taken out is remembered only while "
           "something can bring it back")
{
    Kept k;
    const auto value = k.get (k.clip);
    auto element = k.element (k.clip);

    {
        const phazite::Song::OutsideHistory outside (*k.song);
        element.getParent().removeChild (element, nullptr);
    }

    CHECK_FALSE (k.song->getUndoManager().canUndo());
    CHECK (k.find (k.clip) == nullptr);
    CHECK (k.song->getValues()->remembers (element)); // held here

    // The same element put back: its value back.
    {
        const phazite::Song::OutsideHistory outside (*k.song);
        k.song->getState().getChildWithName (schema::TRACKS).getChild (1).appendChild (element, nullptr);
    }

    CHECK_FALSE (k.song->getUndoManager().canUndo());
    CHECK (k.find (k.clip)->isSameAs (value));
    CHECK_FALSE (k.song->getValues()->remembers (element));
    phazite::test::checkValues (*k.song);

    // Deleted in a step, then the history cleared: what could bring it back is gone, and so is what was remembered, as the
    // next changes look.
    // While its step is still open, the clip's value is out of the area already, the keeper remembering it: the check of
    // the song's values, which allows no value of a clip outside the tree, holds there too.
    k.step ("Delete Clip",
            [&]
            {
                phazite::deleteClips (*k.song, { k.clip }, false);
                CHECK (k.find (k.clip) == nullptr);
                CHECK (k.song->getValues()->remembers (element));
                phazite::test::checkValues (*k.song);
            });
    element = {};
    k.song->getUndoManager().clearUndoHistory();

    for (int i = 0; i < 70; ++i)
    {
        const auto made =
            phazite::insertMidiClip (*k.song, k.tracks[2], { phazite::test::at (i * 4.0), phazite::test::at (i * 4.0 + 2.0) }, nullptr);
        phazite::deleteClips (*k.song, { made }, false);
    }

    k.song->getUndoManager().clearUndoHistory();
    const auto last = phazite::insertMidiClip (*k.song, k.tracks[2], { phazite::test::at (400.0), phazite::test::at (402.0) }, nullptr);
    CHECK (k.find (last) != nullptr);
    phazite::test::checkValues (*k.song);
}

TEST_CASE ("Song values: its listeners are told the clips whose values changed, by a step, an undo and a redo alike")
{
    Kept k;
    k.told.told.clear();

    {
        const phazite::UndoStep step (*k.song, "Add Note");
        phazite::addNote (*k.song, k.clip, 72, phazite::test::at (4.0), phazite::test::lasting (1.0), 100);
    }

    REQUIRE (k.told.told.size() == 1);
    CHECK (k.told.told[0] == std::vector<ClipID> { k.clip });

    phazite::undo (*k.song);
    phazite::redo (*k.song);
    REQUIRE (k.told.told.size() == 3);
    CHECK (k.told.told[1] == std::vector<ClipID> { k.clip });
    CHECK (k.told.told[2] == std::vector<ClipID> { k.clip });

    // A change of the tree alone tells nothing.
    {
        const phazite::UndoStep step (*k.song, "Rename");
        phazite::renameTrack (*k.song, k.tracks[0], "Lead");
    }

    CHECK (k.told.told.size() == 3);
}

//==============================================================================
// A clip as a value (SongValues.h: ClipValue), which the song doesn't hold yet: the conversion between it and a CLIP element.
namespace
{
// Every clip of the golden projects that hold clips, as the song holds each once it is open (its loader's checks made), whole
// (WholeState.h), with its project open.
struct GoldenClipElements
{
    phazite::test::TemporaryFolder temporary;
    std::vector<std::unique_ptr<phazite::Project>> projects;
    std::vector<juce::ValueTree> wholes, clips; // each song whole, and its clips in it

    GoldenClipElements()
    {
        for (const auto* name : { "projects/representative.xml", "commands/script.xml" })
        {
            const auto file = temporary.folder.getChildFile (juce::String (projects.size()) + ".phazite");
            REQUIRE (phazite::test::getGoldenFolder().getChildFile (name).copyFileTo (file));
            juce::String error;
            auto project = phazite::Project::open (file, false, error);
            REQUIRE_MESSAGE (project != nullptr, error.toStdString());

            // Each clip as the song would save it (WholeState.h), in its track's copy.
            const std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& element)
            {
                for (const auto& child : element)
                {
                    if (child.hasType (schema::CLIP))
                        clips.push_back (child);
                    else if (child.hasType (schema::TRACKS) || child.hasType (schema::TRACK))
                        collect (child);
                }
            };

            wholes.push_back (phazite::test::copyWholeSong (project->getSong())); // kept: the clips' tracks are its elements
            collect (wholes.back());

            projects.push_back (std::move (project));
        }
    }
};

// The properties one element holds and the other hasn't, or holds otherwise, by name: "name" for one only the first holds.
std::vector<juce::String> getDifferences (const juce::ValueTree& first, const juce::ValueTree& second)
{
    std::vector<juce::String> differences;

    for (int i = 0; i < first.getNumProperties(); ++i)
        if (const auto name = first.getPropertyName (i); ! second.hasProperty (name) || ! (second[name] == first[name]))
            differences.push_back (name.toString());

    for (int i = 0; i < second.getNumProperties(); ++i)
        if (const auto name = second.getPropertyName (i); ! first.hasProperty (name))
            differences.push_back (name.toString() + " (written)");

    return differences;
}

// A copy of an element without its properties and its WARP children.
juce::ValueTree emptied (const juce::ValueTree& clip)
{
    auto copy = clip.createCopy();
    copy.removeAllProperties (nullptr);

    for (int i = copy.getNumChildren(); --i >= 0;)
        if (copy.getChild (i).hasType (schema::WARP))
            copy.removeChild (i, nullptr);

    return copy;
}

phazite::ClipValue makeClip (std::uint64_t id, std::uint64_t track, std::uint64_t link = 0)
{
    phazite::ClipValue clip;
    clip.id = ClipID::fromRaw (id);
    clip.track = phazite::TrackID::fromRaw (track);
    clip.link = link;
    return clip;
}

std::vector<std::uint64_t> getList (const immer::map<std::uint64_t, immer::flex_vector<ClipID>>& lists, std::uint64_t key)
{
    std::vector<std::uint64_t> ids;

    if (const auto* list = lists.find (key))
        for (const auto& id : *list)
            ids.push_back (id.getRaw());

    return ids;
}
} // namespace

TEST_CASE ("Song values: every clip of the golden projects, read into a value and written into an emptied copy, is the clip")
{
    GoldenClipElements golden;
    REQUIRE (golden.clips.size() >= 10);
    auto audio = 0, warped = 0, faded = 0, shaped = 0, linked = 0, taken = 0;

    for (const auto& clip : golden.clips)
    {
        const auto value = phazite::readClipValue (clip);
        CHECK (value.id == ClipID::fromRaw (phazite::readID (clip[schema::clipID])));
        CHECK (value.track == phazite::getTrackID (clip.getParent()));
        CHECK (value.isAudio() == (clip[schema::clipKind].toString() == schema::clipIsAudio));
        audio += value.isAudio() ? 1 : 0;
        warped += value.sound.warps.empty() ? 0 : 1;
        faded += value.fadeIn.seconds > 0.0 ? 1 : 0;
        shaped += value.fadeIn.sCurve ? 1 : 0;
        linked += value.link != 0 ? 1 : 0;
        taken += value.takes > 0 ? 1 : 0;

        auto written = emptied (clip);
        phazite::writeClipElement (value, written, nullptr);

        // Read back, the same value (but its track: the copy is on none).
        auto again = phazite::readClipValue (written);
        again.track = value.track;
        CHECK (again == value);

        // The same element, property for property: written as the clips' code writes it.
        const auto differences = getDifferences (clip, written);
        CHECK_MESSAGE (differences.empty(), (juce::StringArray (differences.data(), (int) differences.size()).joinIntoString (", ")
                                             + " differ in " + clip.toXmlString())
                                                .toStdString());
        CHECK_MESSAGE (written.isEquivalentTo (clip), clip.toXmlString().toStdString());
    }

    // What the projects hold: every kind of clip and of what a clip holds.
    CHECK (audio >= 4);
    CHECK (warped >= 1);
    CHECK (faded >= 2);
    CHECK (shaped >= 1);
    CHECK (linked >= 2);
    CHECK (taken >= 2);
}

TEST_CASE ("Song values: a clip as each of the clips' makers and edits writes it, read into a value and written into an empty "
           "element, is the clip, property for property")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto audioTrack = s.addTrack ("Audio");

    // Written into an element without an ID, as a new clip is; a maker's own clip in its maker's order too.
    const auto writtenAsMade = [&song] (phazite::ClipID clip, bool inOrder)
    {
        const auto made = phazite::copyWholeClip (song, clip); // as the song would save it
        REQUIRE (made.isValid());
        juce::ValueTree written (schema::CLIP);
        phazite::writeClipElement (phazite::readClipValue (made), written, nullptr);
        const auto differences = getDifferences (made, written);
        CHECK_MESSAGE (differences.empty(), (juce::StringArray (differences.data(), (int) differences.size()).joinIntoString (", ")
                                             + " differ in " + made.toXmlString())
                                                .toStdString());
        CHECK_MESSAGE (written.isEquivalentTo (made), (made.toXmlString() + written.toXmlString()).toStdString());

        if (inOrder)
            for (int i = 0; i < made.getNumProperties(); ++i)
                CHECK (written.getPropertyName (i) == made.getPropertyName (i));
    };

    // A MIDI clip inserted, with a name and without one (called after its track).
    writtenAsMade (phazite::insertMidiClip (song, s.firstTrack, { phazite::test::at (2.0), phazite::test::at (6.0) }, nullptr, "Bass"),
                   true);
    writtenAsMade (phazite::insertMidiClip (song, s.firstTrack, { phazite::test::at (8.0), phazite::test::at (8.001) }, nullptr), true);

    // An audio clip imported.
    const auto imported =
        s.addAudioClip (audioTrack, phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Imported.wav"), 2.0), 4.0);
    writtenAsMade (imported, true);

    // An audio clip recorded round a loop three times, the last time playing, as the recorder makes it (it follows the
    // song's tempo, its tempo held steady); then given fades of both shapes, a pitch, a gain and warp markers, reversed
    // and playing its first take.
    const auto second = (std::int64_t) 48000; // the song's samples, at the take's rate, from 4 seconds (beat 8) round 1 second
    phazite::TakePlace take;
    take.sampleRate = (double) second;
    take.frames = 3 * second;
    take.firstFrame = take.start = 4 * second;
    take.stop = 7 * second;
    take.loop = phazite::TakeLoop { 4 * second, 5 * second };
    const auto file = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 3.0);
    REQUIRE (phazite::placeTake (song, audioTrack, file, take).size() == 1);
    const auto recorded = phazite::getClips (song, audioTrack).back();
    REQUIRE (phazite::getClipTakeCount (song, recorded) == 3);
    REQUIRE (phazite::getClipTempo (song, recorded) > 0.0);
    writtenAsMade (recorded, true);

    phazite::setClipFade (song, recorded, true, 0.1);
    phazite::setClipFade (song, recorded, false, 0.2);
    phazite::setFadeCurve (song, recorded, true, { phazite::FadeShape::sCurve, 0.0f });
    phazite::setFadeCurve (song, recorded, false, { phazite::FadeShape::curve, -0.5f });
    phazite::setClipPitch (song, recorded, 3.0f);
    phazite::setClipGain (song, recorded, -4.5f);
    CHECK (phazite::addWarpMarker (song, recorded, 2.25));
    CHECK (phazite::addWarpMarker (song, recorded, 2.5));
    phazite::setClipReversed (song, recorded, true);
    phazite::playTake (song, recorded, 1);

    const auto edited = phazite::getClipValue (song, recorded).value();
    CHECK (edited.fadeIn.sCurve);
    CHECK (juce::exactlyEqual (edited.fadeOut.bend, -0.5f));
    CHECK (edited.sound.reversed);
    CHECK (edited.sound.warps.size() == 2);
    CHECK (edited.take == 1);
    CHECK (juce::exactlyEqual (edited.gain, -4.5f));
    writtenAsMade (recorded, false);
}

TEST_CASE ("Song values: a clip's value written into its element changes only what differs, and leaves what isn't the value's")
{
    juce::ValueTree track (schema::TRACK);
    track.setProperty (schema::trackID, 5, nullptr);
    juce::ValueTree clip (schema::CLIP);
    track.appendChild (clip, nullptr);

    auto value = makeClip (7, 5);
    value.kind = phazite::ClipValue::Kind::audio;
    value.length = {}; // an audio clip's element holds none, which reads as 0
    value.name = "Loop";
    value.start = phazite::test::at (4.0);
    value.sound.file = "Audio/Loop.wav";
    value.sound.tempo = 120.0;
    value.sound.warps = { { 0.5, 0.5 }, { 1.0, 1.25 } };
    value.fileEnd = 2.0;
    value.fadeOut = { 0.25, true, -0.5f };
    phazite::writeClipElement (value, clip, nullptr);
    clip.setProperty ("unknown", 1, nullptr);
    clip.appendChild (juce::ValueTree ("SOMETHING"), nullptr);
    CHECK (phazite::readClipValue (clip) == value);
    CHECK_FALSE (clip.hasProperty (schema::clipLength)); // an audio clip's length follows from its part
    CHECK_FALSE (clip.hasProperty (schema::clipFadeIn)); // at its default

    Heard heard;
    track.addListener (&heard);
    const auto forget = [&heard] { heard.properties = heard.added = heard.removed = heard.moved = 0; };
    const auto writeAndCount = [&] (const phazite::ClipValue& next)
    {
        forget();
        phazite::writeClipElement (next, clip, nullptr);
        CHECK (phazite::readClipValue (clip) == next);
        return heard.count();
    };

    CHECK (writeAndCount (value) == 0);

    auto gained = value;
    gained.gain = 3.0f;
    CHECK (writeAndCount (gained) == 1);

    auto moved = gained;
    moved.sound.warps = moved.sound.warps.set (1, { 1.0, 1.5 });
    CHECK (writeAndCount (moved) == 1);

    auto unwarped = moved;
    unwarped.sound.warps = {};
    unwarped.sound.tempo = 0.0;
    CHECK (writeAndCount (unwarped) == 3); // the tempo and both markers

    // Its name taken away, as Rename takes it away when a clip is named as its track; then none written, and an empty one
    // held, as an audio clip is made with, stays.
    auto unnamed = unwarped;
    unnamed.name = {};
    CHECK (writeAndCount (unnamed) == 1);
    CHECK_FALSE (clip.hasProperty (schema::clipName));
    CHECK (writeAndCount (unnamed) == 0);
    CHECK_FALSE (clip.hasProperty (schema::clipName));
    clip.setProperty (schema::clipName, juce::String(), nullptr);
    CHECK (writeAndCount (unnamed) == 0);
    CHECK (clip.hasProperty (schema::clipName));

    // A start before the song's kept at its start, as setClipPlace() and setAudioPart() keep it; a part within its file.
    auto early = unnamed;
    early.start = phazite::BeatPosition::fromBeats (-2.0);
    early.fileStart = -1.0;
    early.fileEnd = -2.0;
    phazite::writeClipElement (early, clip, nullptr);
    CHECK (juce::exactlyEqual ((double) clip[schema::clipStart], 0.0));
    CHECK (juce::exactlyEqual ((double) clip[schema::clipFileStart], 0.0));
    CHECK (juce::exactlyEqual ((double) clip[schema::clipFileEnd], 0.0));

    // A MIDI clip of it: a MIDI clip's length comes, at least as long as the clips' code keeps a clip, and what only an audio
    // clip holds stays as it is (the clips' code never writes it on a MIDI clip); and back, the MIDI clip's length stays.
    auto midi = unwarped;
    midi.kind = phazite::ClipValue::Kind::midi;
    midi.length = phazite::test::lasting (1.0e-4);
    midi.offset = phazite::test::lasting (1.0);
    phazite::writeClipElement (midi, clip, nullptr);
    CHECK (juce::exactlyEqual ((double) clip[schema::clipLength], phazite::shortestClipBeats));
    CHECK (juce::exactlyEqual ((double) clip[schema::clipOffset], 1.0));
    CHECK (clip[schema::clipFile].toString() == juce::String ("Audio/Loop.wav"));
    CHECK (juce::exactlyEqual ((double) clip[schema::clipGain], 3.0));
    CHECK (clip.hasProperty (schema::clipFadeOut));

    phazite::writeClipElement (unwarped, clip, nullptr);
    CHECK (juce::exactlyEqual ((double) clip[schema::clipLength], phazite::shortestClipBeats));
    CHECK (phazite::readClipValue (clip).length == phazite::BeatDuration()); // an audio clip's value has no length

    // A value's length is none of an audio clip's value, whatever it holds.
    auto longer = unwarped;
    longer.length = phazite::test::lasting (8.0);
    CHECK (longer == unwarped);
    longer.kind = phazite::ClipValue::Kind::midi;
    auto shorter = longer;
    shorter.length = phazite::test::lasting (2.0);
    CHECK_FALSE (shorter == longer);

    // What isn't a clip's value stays.
    CHECK ((int) clip["unknown"] == 1);
    CHECK (clip.getChildWithName ("SOMETHING").isValid());
    track.removeListener (&heard);
}

TEST_CASE ("Song values: a property of a clip read as its element would hold it, and one a clip's element hasn't reads as it "
           "always has")
{
    juce::ValueTree clip (schema::CLIP);
    clip.setProperty (schema::clipID, 3, nullptr);
    clip.setProperty (schema::clipKind, schema::clipIsAudio, nullptr);
    clip.setProperty (schema::clipFadeInBend, 4.0, nullptr); // beyond its range: kept within it, as the clips' code reads it
    const auto value = phazite::readClipValue (clip);
    CHECK (value.isAudio());
    CHECK (value.length == phazite::BeatDuration()); // absent: 0
    CHECK (value.take == 1);
    CHECK (juce::exactlyEqual (value.fadeIn.bend, 1.0f));
    CHECK_FALSE (value.track.isValid());

    auto chosen = value;
    CHECK (phazite::setClipProperty (chosen, schema::clipGain, 6));
    CHECK (phazite::setClipProperty (chosen, schema::clipMute, true));
    CHECK (phazite::setClipProperty (chosen, schema::clipTake, juce::var()));
    CHECK (phazite::setClipProperty (chosen, schema::clipFadeOutShape, schema::fadeIsSCurve));
    CHECK (juce::exactlyEqual (chosen.gain, 6.0f));
    CHECK (chosen.muted);
    CHECK (chosen.take == 1);
    CHECK (chosen.fadeOut.sCurve);
    CHECK_FALSE (phazite::setClipProperty (chosen, schema::noteVelocity, 60)); // a note's
    CHECK_FALSE (phazite::setClipProperty (chosen, "unknown", 1));
}

TEST_CASE ("Song values: every clip of a song by its ID, its track's list and its group's, kept together by the helpers")
{
    using phazite::SongClips;
    SongClips clips;
    CHECK (phazite::holdsItsLists (clips));

    // Three on track 1, one at a place among them; one on track 2.
    clips = phazite::withClip (clips, makeClip (10, 1));
    clips = phazite::withClip (clips, makeClip (11, 1));
    clips = phazite::withClip (clips, makeClip (12, 1), 1);
    clips = phazite::withClip (clips, makeClip (20, 2));
    CHECK (phazite::holdsItsLists (clips));
    CHECK (getList (clips.tracks, 1) == std::vector<std::uint64_t> { 10u, 12u, 11u });
    CHECK (getList (clips.tracks, 2) == std::vector<std::uint64_t> { 20u });
    REQUIRE (clips.find (ClipID::fromRaw (12)) != nullptr);
    CHECK (clips.find (ClipID::fromRaw (13)) == nullptr);

    // Changed in place: it keeps its place; moved to another track: the end of that one's list, the old one's without it.
    auto renamed = *clips.find (ClipID::fromRaw (12));
    renamed.name = "Renamed";
    const auto before = clips;
    clips = phazite::withClip (clips, renamed);
    CHECK_FALSE (clips.isSameAs (before));
    CHECK (clips.tracks.identity() == before.tracks.identity());
    CHECK (getList (clips.tracks, 1) == std::vector<std::uint64_t> { 10u, 12u, 11u });

    auto moved = renamed;
    moved.track = phazite::TrackID::fromRaw (2);
    clips = phazite::withClip (clips, moved);
    CHECK (phazite::holdsItsLists (clips));
    CHECK (getList (clips.tracks, 1) == std::vector<std::uint64_t> { 10u, 11u });
    CHECK (getList (clips.tracks, 2) == std::vector<std::uint64_t> { 20u, 12u });

    // A clip's link set or changed: it leaves its group's list (if in one) for the end of the new group's.
    const auto linkTo = [&clips] (std::uint64_t id, std::uint64_t link)
    {
        auto linked = *clips.find (ClipID::fromRaw (id));
        linked.link = link;
        clips = phazite::withClip (clips, linked);
    };

    for (const auto id : { 10u, 20u, 12u })
        linkTo (id, 99u);

    linkTo (11u, 98u);
    CHECK (phazite::holdsItsLists (clips));
    CHECK (phazite::hasGroupsOfOne (clips)); // 98, of 11 alone, so far
    CHECK (getList (clips.groups, 99) == std::vector<std::uint64_t> { 10u, 20u, 12u });

    linkTo (12u, 98u); // from one group to another
    CHECK (phazite::holdsItsLists (clips));
    CHECK_FALSE (phazite::hasGroupsOfOne (clips));
    CHECK (getList (clips.groups, 99) == std::vector<std::uint64_t> { 10u, 20u });
    CHECK (getList (clips.groups, 98) == std::vector<std::uint64_t> { 11u, 12u });
    CHECK (clips.tracks.identity() == phazite::withClip (clips, *clips.find (ClipID::fromRaw (12))).tracks.identity());

    // One made unique leaves a group of one, which the helpers keep (it is the caller's to dissolve, as the model's
    // removeClipState() does): the last one unlinked, no group is left.
    linkTo (20u, 0u);
    CHECK (phazite::holdsItsLists (clips));
    CHECK (phazite::hasGroupsOfOne (clips));
    CHECK (getList (clips.groups, 99) == std::vector<std::uint64_t> { 10u });
    linkTo (10u, 0u);
    CHECK (clips.groups.find (99u) == nullptr);
    CHECK_FALSE (phazite::hasGroupsOfOne (clips));

    // The next-to-last one of a group deleted: the same, the last one then unlinked by the caller.
    clips = phazite::withoutClip (clips, ClipID::fromRaw (12));
    CHECK (phazite::holdsItsLists (clips));
    CHECK (phazite::hasGroupsOfOne (clips));
    CHECK (getList (clips.groups, 98) == std::vector<std::uint64_t> { 11u });
    CHECK (getList (clips.tracks, 2) == std::vector<std::uint64_t> { 20u });
    CHECK (clips.find (ClipID::fromRaw (12)) == nullptr);
    CHECK (phazite::withoutClip (clips, ClipID::fromRaw (12)).isSameAs (clips)); // gone already
    linkTo (11u, 0u);
    CHECK (clips.groups.empty());
    CHECK_FALSE (phazite::hasGroupsOfOne (clips));

    clips = phazite::withoutClip (clips, ClipID::fromRaw (20));
    CHECK (clips.tracks.find (2u) == nullptr); // a track without clips has no list
    CHECK (phazite::holdsItsLists (clips));

    // A track's clips in another order; and an order of other clips than the track's refused, the clips as they were.
    clips = phazite::withClip (clips, makeClip (13, 1));
    clips = phazite::withTrackOrder (clips, phazite::TrackID::fromRaw (1),
                                     { ClipID::fromRaw (13), ClipID::fromRaw (11), ClipID::fromRaw (10) });
    CHECK (getList (clips.tracks, 1) == std::vector<std::uint64_t> { 13u, 11u, 10u });
    CHECK (phazite::holdsItsLists (clips));

    {
        const phazite::test::ExpectedAssertion refused ("SongValues.cpp");
        const auto track = phazite::TrackID::fromRaw (1);
        CHECK (phazite::withTrackOrder (clips, track, { ClipID::fromRaw (13), ClipID::fromRaw (10) }).isSameAs (clips));
        CHECK (phazite::withTrackOrder (clips, track,
                                        { ClipID::fromRaw (13), ClipID::fromRaw (11), ClipID::fromRaw (10), ClipID::fromRaw (14) })
                   .isSameAs (clips));
        CHECK (
            phazite::withTrackOrder (clips, track, { ClipID::fromRaw (13), ClipID::fromRaw (13), ClipID::fromRaw (10) }).isSameAs (clips));
#if JUCE_DEBUG
        CHECK (refused.getCount() == 3);
#endif
    }

    // A clip moved into another track at a place among its clips: there, and out of its old track's list.
    clips = phazite::withClip (clips, makeClip (21u, 2u));
    clips = phazite::withClip (clips, makeClip (22u, 2u));
    auto placed = *clips.find (ClipID::fromRaw (11));
    placed.track = phazite::TrackID::fromRaw (2);
    clips = phazite::withClip (clips, placed, 1);
    CHECK (phazite::holdsItsLists (clips));
    CHECK (getList (clips.tracks, 1) == std::vector<std::uint64_t> { 13u, 10u });
    CHECK (getList (clips.tracks, 2) == std::vector<std::uint64_t> { 21u, 11u, 22u });

    // The three disagreeing: a clip on a track whose list hasn't it, a list holding a clip twice, a group's list one of another.
    auto broken = clips;
    broken.clips = broken.clips.set (30u, makeClip (30, 1));
    CHECK_FALSE (phazite::holdsItsLists (broken));
    broken = clips;
    broken.tracks = broken.tracks.set (1u, broken.tracks[1u].push_back (ClipID::fromRaw (10)));
    CHECK_FALSE (phazite::holdsItsLists (broken));
    broken = clips;
    broken.groups = broken.groups.set (98u, { ClipID::fromRaw (10) });
    CHECK_FALSE (phazite::holdsItsLists (broken));
}

TEST_CASE ("Song values: the song's clips as an area of the song's, changed in a step and taken back and forth whole")
{
    auto song = phazite::createSong();
    phazite::ValueArea<phazite::SongClips> area (song->getUndoManager(), {});
    const auto start = area.get();

    {
        const phazite::UndoStep step (*song, "Add Clips");
        area.set (phazite::withClip (area.get(), makeClip (1, 7, 5)));
        area.set (phazite::withClip (area.get(), makeClip (2, 7, 5)));
    }

    const auto added = area.get();
    CHECK (area.countActions() == 1);
    CHECK (phazite::holdsItsLists (added));
    CHECK (getList (added.groups, 5) == std::vector<std::uint64_t> { 1u, 2u });

    phazite::undo (*song);
    CHECK (area.get().isSameAs (start));
    phazite::redo (*song);
    CHECK (area.get().isSameAs (added));

    // The same value set again is no change.
    area.set (added);
    CHECK (area.countActions() == 1);
}

TEST_CASE ("Song values: a song that keeps everything in its tree is read from it, and copied whole as it is")
{
    auto made = phazite::createSong();
    const auto track = phazite::ensureTracks (*made, 1).front();
    const auto clip = phazite::insertMidiClip (*made, track, { phazite::test::at (0.0), phazite::test::at (4.0) }, nullptr);
    phazite::addNote (*made, clip, 60, phazite::test::at (1.0), phazite::test::lasting (1.0), 100);

    // As the versions' checks make one on another thread, from a copy: no values held.
    phazite::Song song (phazite::copyWholeSong (*made));
    CHECK (song.getValues() == nullptr);
    CHECK (phazite::getClipNotesValue (song, clip).notes.size() == 1);
    CHECK (phazite::getClipNotes (song, clip).size() == 1);
    CHECK (phazite::copyWholeSong (song).isEquivalentTo (phazite::copyWholeSong (*made)));
    CHECK (phazite::test::findValueProblems (song).empty());
}
