// SPDX-License-Identifier: AGPL-3.0-or-later
// A MIDI clip's notes and controllers' values as a value (model/base/internal/SongValues.h: ClipNotes), and the conversion
// between the value and the clip's element, the one place that reads and writes NOTE and CONTROL by name: a clip read into
// a value and written back is the clip it was; a changed value changes only what differs; the helpers that change a value in
// batches keep where each note is by its ID; what isn't a note or a value of a kind Phazite knows stays as it is.
#include "GoldenFiles.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"

#include <vector>

namespace
{
namespace schema = phazite::schema;
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

NoteValues makeNote (std::uint64_t id, int pitch, double start)
{
    return { .id = NoteID::fromRaw (id), .pitch = pitch, .start = phazite::BeatPosition::fromBeats (start) };
}

ControlValues makeControl (double beat, int value)
{
    return { .kind = ControlKind::controller, .number = 64, .beat = beat, .value = value };
}

// A clip's element with three notes and three controllers' values, interleaved as a recording can leave them.
juce::ValueTree makeClipElement()
{
    juce::ValueTree clip (schema::CLIP);
    clip.setProperty (schema::clipKind, schema::clipIsMidi, nullptr);
    phazite::writeClipNotes (phazite::makeClipNotes ({ makeNote (1, 60, 0.0) }, { makeControl (0.0, 127) }), clip, nullptr);
    phazite::writeClipNotes (
        phazite::makeClipNotes ({ makeNote (1, 60, 0.0), makeNote (2, 62, 1.0) }, { makeControl (0.0, 127), makeControl (0.5, 0) }), clip,
        nullptr);
    phazite::writeClipNotes (phazite::makeClipNotes ({ makeNote (1, 60, 0.0), makeNote (2, 62, 1.0), makeNote (3, 64, 2.0) },
                                                     { makeControl (0.0, 127), makeControl (0.5, 0), makeControl (1.5, 127) }),
                             clip, nullptr);
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

            for (const auto& clip : phazite::getClipStates (project->getSong()))
                if (phazite::isMidiState (clip) && clip.getNumChildren() > 0)
                    clips.push_back (phazite::copyWhole (project->getSong(), clip));

            projects.push_back (std::move (project));
        }
    }
};
} // namespace

TEST_CASE ("Song values: every clip of the golden projects, read into a value and written into an emptied copy, is the clip it was")
{
    GoldenClips golden;
    REQUIRE (golden.clips.size() >= 4);
    auto interleaved = 0;

    for (const auto& clip : golden.clips)
    {
        const auto value = phazite::readClipNotes (clip);
        CHECK (value.holdsItsPlaces());
        CHECK ((int) (value.notes.size() + value.controls.size()) == clip.getNumChildren());

        // Emptied, then given its value: its notes and its values each in their order, after what else it holds.
        auto written = clip.createCopy();
        phazite::writeClipNotes ({}, written, nullptr);
        REQUIRE (written.getNumChildren() == 0);
        phazite::writeClipNotes (value, written, nullptr);
        CHECK (phazite::readClipNotes (written) == value);

        // And in the clip's order of notes and values among each other, which the value doesn't hold.
        if (! written.isEquivalentTo (clip))
            ++interleaved;

        phazite::followOrderOf (written, clip, nullptr);
        CHECK_MESSAGE (written.isEquivalentTo (clip), clip.toXmlString().toStdString());
        CHECK (getChildTypes (written) == getChildTypes (clip));

        // Written into the clip itself, its value changes nothing at all, its children where they were.
        auto same = clip.createCopy();
        Heard heard;
        same.addListener (&heard);
        phazite::writeClipNotes (value, same, nullptr);
        same.removeListener (&heard);
        CHECK (heard.count() == 0);
        CHECK (same.isEquivalentTo (clip));
    }

    CHECK (interleaved > 0); // the representative project's controller clip, its values before its notes
}

TEST_CASE ("Song values: a clip's notes read into a value and written back are the clip's element as it was")
{
    // Every value a note can have, on 200 notes made by the model.
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
    const auto whole = phazite::copyWhole (*song, phazite::findClipState (*song, clip));
    const auto value = phazite::getClipNotesValue (*song, clip);
    REQUIRE (value.notes.size() == 200);
    CHECK (value == phazite::readClipNotes (whole));

    auto written = whole.createCopy();
    phazite::writeClipNotes ({}, written, nullptr);
    REQUIRE_FALSE (written.isEquivalentTo (whole));
    phazite::writeClipNotes (value, written, nullptr);
    CHECK (written.isEquivalentTo (whole));
}

TEST_CASE ("Song values: a changed value writes only what differs, each change once")
{
    auto clip = makeClipElement();
    const auto before = getChildTypes (clip);
    REQUIRE (before == std::vector<juce::String> { "NOTE1", "CONTROL", "NOTE2", "CONTROL", "NOTE3", "CONTROL" });
    const auto value = phazite::readClipNotes (clip);

    const auto heardFor = [&clip] (const ClipNotes& next)
    {
        auto copy = clip.createCopy();
        Heard heard;
        copy.addListener (&heard);
        phazite::writeClipNotes (next, copy, nullptr);
        copy.removeListener (&heard);
        CHECK (phazite::readClipNotes (copy) == next);
        return std::pair { heard, getChildTypes (copy) };
    };

    // A note's velocity: one property.
    {
        const auto [heard, types] =
            heardFor (phazite::withNotesChanged (value, { NoteID::fromRaw (2) }, [] (NoteValues& note) { note.velocity = 80; }));
        CHECK (heard.properties == 1);
        CHECK (heard.count() == 1);
        CHECK (types == before);
    }

    // A note's flag turned on: one property; and its channel: one more.
    {
        const auto [heard, types] = heardFor (phazite::withNotesChanged (value, { NoteID::fromRaw (3) },
                                                                         [] (NoteValues& note)
                                                                         {
                                                                             note.muted = true;
                                                                             note.channel = 4;
                                                                         }));
        CHECK (heard.properties == 2);
        CHECK (heard.count() == 2);
        CHECK (types == before);
    }

    // A note taken out: one removal, the others where they were.
    {
        const auto [heard, types] =
            heardFor (phazite::withoutNotes (value, [] (const NoteValues& note) { return note.id == NoteID::fromRaw (2); }));
        CHECK (heard.removed == 1);
        CHECK (heard.count() == 1);
        CHECK (types == std::vector<juce::String> { "NOTE1", "CONTROL", "CONTROL", "NOTE3", "CONTROL" });
    }

    // A note added: after all the clip holds, as a note added always went.
    {
        const auto [heard, types] = heardFor (phazite::withNoteAdded (value, makeNote (9, 70, 3.0)));
        CHECK (heard.added == 1);
        CHECK (heard.count() == 1);
        CHECK (types == std::vector<juce::String> { "NOTE1", "CONTROL", "NOTE2", "CONTROL", "NOTE3", "CONTROL", "NOTE9" });
    }

    // A note taken out and put back with its ID after the others (a replaced note, as replaceNotes() makes it): taken out
    // and added, after all the clip holds.
    {
        auto next = phazite::withoutNotes (value, [] (const NoteValues& note) { return note.id == NoteID::fromRaw (1); });
        next = phazite::withNoteAdded (next, makeNote (1, 61, 0.25));
        const auto [heard, types] = heardFor (next);
        CHECK (heard.removed == 1);
        CHECK (heard.added == 1);
        CHECK (heard.count() == 2);
        CHECK (types == std::vector<juce::String> { "CONTROL", "NOTE2", "CONTROL", "NOTE3", "CONTROL", "NOTE1" });
    }

    // A controller's value changed: one property, in its place.
    {
        const auto [heard, types] = heardFor (phazite::withEachChanged (
            value, [] (NoteValues&) {},
            [] (ControlValues& control)
            {
                if (juce::exactlyEqual (control.beat, 0.5))
                    control.value = 100;
            }));
        CHECK (heard.properties == 1);
        CHECK (heard.count() == 1);
        CHECK (types == before);
    }

    // One taken out and another added (a step of a draw in a lane): one removal and one addition, the others where they were.
    {
        auto next = phazite::withoutControls (value, [] (size_t place, const ControlValues&) { return place == 1; });
        next = phazite::withControlsAdded (next, { makeControl (3.0, 64) });
        const auto [heard, types] = heardFor (next);
        CHECK (heard.removed == 1);
        CHECK (heard.added == 1);
        CHECK (heard.count() == 2);
        CHECK (types == std::vector<juce::String> { "NOTE1", "CONTROL", "NOTE2", "NOTE3", "CONTROL", "CONTROL" });
    }

    // Every value's beat changed (a tempo change that keeps the patterns' speed): each in its place, one property each.
    {
        const auto [heard, types] =
            heardFor (phazite::withEachChanged (value, [] (NoteValues&) {}, [] (ControlValues& control) { control.beat += 0.125; }));
        CHECK (heard.properties == 3);
        CHECK (heard.count() == 3);
        CHECK (types == before);
    }

    // Nothing changed: nothing heard.
    {
        const auto [heard, types] = heardFor (value);
        CHECK (heard.count() == 0);
        CHECK (types == before);
    }
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
    phazite::writeClipNotes (value, clip, nullptr);
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

TEST_CASE ("Song values: a clip's other children, and a value of a kind Phazite doesn't know, stay where they are")
{
    auto clip = makeClipElement();
    clip.addChild (juce::ValueTree ("SOMETHING", { { "a", 1 } }), 1, nullptr);
    clip.addChild (juce::ValueTree (schema::CONTROL, { { schema::controlKind, "theremin" }, { schema::controlBeat, 0.75 } }), 4, nullptr);
    const auto before = clip.createCopy();

    const auto value = phazite::readClipNotes (clip);
    CHECK (value.notes.size() == 3);
    CHECK (value.controls.size() == 3);

    auto next = phazite::withNotesChanged (value, { NoteID::fromRaw (1) }, [] (NoteValues& note) { note.pitch = 30; });
    next = phazite::withoutNotes (next, [] (const NoteValues& note) { return note.id == NoteID::fromRaw (2); });
    next = phazite::withoutControls (next, [] (size_t place, const ControlValues&) { return place == 0; });
    next = phazite::withNoteAdded (next, makeNote (5, 72, 3.0));
    phazite::writeClipNotes (next, clip, nullptr);

    CHECK (phazite::readClipNotes (clip) == next);
    CHECK (clip.getChild (1).isEquivalentTo (before.getChild (1)));
    REQUIRE (clip.getChildWithProperty (schema::controlKind, "theremin").isValid());
    CHECK (clip.getChildWithProperty (schema::controlKind, "theremin").isEquivalentTo (before.getChild (4)));
    CHECK (clip.getChildWithName ("SOMETHING").isEquivalentTo (before.getChild (1)));

    // Taken away whole, the clip keeps them still.
    phazite::writeClipNotes ({}, clip, nullptr);
    CHECK (clip.getNumChildren() == 2);
    CHECK (clip.getChild (0).hasType ("SOMETHING"));
    CHECK (clip.getChild (1)[schema::controlKind].toString() == "theremin");
}

TEST_CASE ("Song values: a note of a project opened from its file, changed through the model, changes in that property alone, the "
           "others as the file wrote them")
{
    // A file's properties are text, which JUCE sees as values of their own beside the numbers they read as ("4" is not 4.0):
    // a write of every property of a note that changed set its start and its length again as numbers, three changes where
    // one was made. The representative project's drum clip (8), its first note (9), as the file wrote it.
    phazite::test::TemporaryFolder temporary;
    const auto file = temporary.folder.getChildFile ("Representative.phazite");
    REQUIRE (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml").copyFileTo (file));
    juce::String error;
    const auto project = phazite::Project::open (file, false, error);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    auto& song = project->getSong();
    const phazite::NoteRef note { phazite::ClipID::fromRaw (8), NoteID::fromRaw (9) };

    const auto noteElement = [&song, &note]
    {
        for (const auto& child : phazite::copyWhole (song, phazite::findClipState (song, note.clip)))
            if (child.hasType (schema::NOTE) && phazite::readNoteValues (child).id == note.note)
                return child;

        return juce::ValueTree();
    };

    const auto before = noteElement();
    REQUIRE (before.isValid());
    REQUIRE (before[schema::noteStart].isString());
    REQUIRE ((int) before[schema::notePitch] == 36);

    struct Changed final : juce::ValueTree::Listener
    {
        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
        {
            changes.push_back (tree.getType().toString() + "." + property.toString());
        }

        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { changes.push_back ("added"); }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { changes.push_back ("removed"); }

        std::vector<juce::String> changes;
    } heard;

    auto root = song.getState();
    root.addListener (&heard);

    {
        const phazite::UndoStep step (song, "Transpose Note");
        phazite::transposeNotes (song, { note }, 2);
    }

    root.removeListener (&heard);
    CHECK (heard.changes == std::vector<juce::String> { "NOTE.pitch" });

    const auto after = noteElement();
    REQUIRE (after.isValid());
    CHECK ((int) after[schema::notePitch] == 38);
    CHECK (after.getNumProperties() == before.getNumProperties());

    for (const auto& property :
         std::vector<juce::Identifier> { schema::noteID, schema::noteStart, schema::noteLength, schema::noteVelocity })
    {
        INFO (property.toString());
        CHECK (after[property].isString() == before[property].isString());
        CHECK (after[property].toString() == before[property].toString());
    }

    for (const auto& property :
         std::vector<juce::Identifier> { schema::noteMute, schema::noteSlide, schema::notePortamento, schema::noteChannel })
        CHECK_FALSE (after.hasProperty (property));
}

TEST_CASE ("Song values: a property a note's element hasn't reads as it always has")
{
    juce::ValueTree note (schema::NOTE);
    note.setProperty (schema::noteID, 7, nullptr);
    const auto values = phazite::readNoteValues (note);
    CHECK (values.id == NoteID::fromRaw (7));
    CHECK (values.pitch == 0);
    CHECK (values.start == phazite::BeatPosition());
    CHECK (values.length == phazite::BeatDuration());
    CHECK (values.velocity == 0);
    CHECK_FALSE (values.muted);
    CHECK (values.channel == 1);

    note.setProperty (schema::noteChannel, 40, nullptr);
    CHECK (phazite::readNoteValues (note).channel == 16);

    // And a value a merge chose for one, as the element would read it.
    auto chosen = values;
    CHECK (phazite::setNoteProperty (chosen, schema::notePitch, 64));
    CHECK (phazite::setNoteProperty (chosen, schema::noteMute, true));
    CHECK (phazite::setNoteProperty (chosen, schema::noteChannel, juce::var()));
    CHECK (chosen.pitch == 64);
    CHECK (chosen.muted);
    CHECK (chosen.channel == 1);
    CHECK_FALSE (phazite::setNoteProperty (chosen, schema::clipName, "x"));
}
