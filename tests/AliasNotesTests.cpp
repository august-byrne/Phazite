// SPDX-License-Identifier: AGPL-3.0-or-later
// The alias rule: the clips of an alias group hold the same notes and controller values (src/model/notes/internal/Notes.h:
// "Each is made to the note in each of its clip's aliases too"; src/model/arrangement/internal/Clips.h: getAliasStates()).
// Every note edit of Notes.h made through a member that isn't the group's first, each one undo step, exact, made once
// (not once for each member), every member the same after it, its Undo and its Redo; a note named through two members in
// one call edited once, whichever is named first; and notes pasted, duplicated and replaced in a member. All through the
// model's functions and commands (and the piano roll's Quantize, on an EditorScreens.h screen), by IDs and values, so that
// these hold however the song keeps its notes.
#include "EditorScreens.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace
{
using phazite::ClipID;
using phazite::ControlKind;
using phazite::ControlLane;
using phazite::NoteData;
using phazite::NoteID;
using phazite::NoteRef;
using phazite::test::at;
using phazite::test::describeBeats;
using phazite::test::getContentStartOf;
using phazite::test::lasting;

// A clip's notes in the order getClipNotes() gives them, all of each (TestClips.h).
juce::String describeNotes (const phazite::Song& song, ClipID clip)
{
    return phazite::test::describeClipNotes (song, clip);
}

// The lanes the tests put values in.
const ControlLane modWheel { ControlKind::controller, 1, 1 };
const ControlLane modWheelTwo { ControlKind::controller, 1, 2 };
const ControlLane bend { ControlKind::pitchWheel, 0, 1 };

// The part of its notes a clip plays, in beats counted from its content start: from its start to its end.
std::pair<double, double> getPlayedBeats (const phazite::Song& song, ClipID clip)
{
    const auto from = getContentStartOf (song, clip);
    const auto range = phazite::getClipRange (song, clip);
    return { (range.getStart() - from).inBeats(), (range.getEnd() - from).inBeats() };
}

// A clip's controller values, lane by lane (getControlLanes()), each at its beat counted from the clip's content start: those
// from one beat of its notes up to another, or all it plays. Notes.h reads a clip's values only where the clip plays them
// (getControlLanes(), getControlValues()): of a value a trimmed clip holds outside what it plays, nothing reads it through
// that clip, so the clips of a group are compared where each plays (haveTheSameControls()).
juce::String describeControls (const phazite::Song& song, ClipID clip, std::pair<double, double> part)
{
    const auto from = getContentStartOf (song, clip);
    juce::StringArray lanes;

    for (const auto& lane : phazite::getControlLanes (song, { clip }))
    {
        juce::StringArray values;

        for (const auto& point : phazite::getControlValues (song, { clip }, lane))
            if (const auto beat = (point.beat - from).inBeats(); beat > part.first - 1.0e-9 && beat < part.second - 1.0e-9)
                values.add (describeBeats (beat) + "=" + juce::String (point.value));

        if (! values.isEmpty())
            lanes.add (lane.getName() + ": " + values.joinIntoString (" "));
    }

    return lanes.joinIntoString ("; ");
}

juce::String describeControls (const phazite::Song& song, ClipID clip)
{
    return describeControls (song, clip, getPlayedBeats (song, clip));
}

// Whether every clip holds the notes the first holds, each with the same values, in the same order.
bool haveTheSameNotes (const phazite::Song& song, const std::vector<ClipID>& clips)
{
    return ! clips.empty()
           && std::all_of (clips.begin(), clips.end(),
                           [&] (ClipID clip) { return describeNotes (song, clip) == describeNotes (song, clips.front()); });
}

// Whether each two of the clips read the same values where both play them: every value of the group that any of them reads.
bool haveTheSameControls (const phazite::Song& song, const std::vector<ClipID>& clips)
{
    for (size_t i = 0; i < clips.size(); ++i)
    {
        for (size_t j = i + 1; j < clips.size(); ++j)
        {
            const auto a = getPlayedBeats (song, clips[i]), b = getPlayedBeats (song, clips[j]);
            const std::pair both { std::max (a.first, b.first), std::min (a.second, b.second) };

            if (both.first < both.second && describeControls (song, clips[i], both) != describeControls (song, clips[j], both))
                return false;
        }
    }

    return ! clips.empty();
}

// Every clip's notes and values, a line each, for a failure's message and for comparing the group before and after.
std::string describeGroup (const phazite::Song& song, const std::vector<ClipID>& clips)
{
    juce::StringArray lines;

    for (const auto clip : clips)
        lines.add ("clip " + juce::String ((juce::int64) clip.getRaw()) + ": " + describeNotes (song, clip) + " | "
                   + describeControls (song, clip));

    return lines.joinIntoString ("\n").toStdString();
}

std::vector<ClipID> sorted (std::vector<ClipID> clips)
{
    std::sort (clips.begin(), clips.end());
    return clips;
}

// A song of two tracks: on the first, a MIDI clip from beat 0 to 4 (its content starts at 0) holding three notes; on the
// second, an alias of it from beat 8 to 12 (its content at 8); and on the first again, an alias that starts at 16 trimmed to
// start a beat in (from 17 to 20, its content at 16, playing its notes from their beat 1). Notes:
//   c: 60 at 0 for 1, velocity 100;  e: 64 at 1 for 1, velocity 90;  g: 67 at 2 for 0.5, velocity 80.
// Edits are made through the second member, onOther, its first alias: never the group's first. Without the snippet, the
// group is of two clips, where an edit made once for each member reads as made twice.
struct Aliased
{
    explicit Aliased (bool withSnippet = true)
    {
        other = song.addTrack ("Other");
        clip = song.addMidiClip (song.firstTrack, 0.0, 4.0);
        c = add (60, 0.0, 1.0, 100);
        e = add (64, 1.0, 1.0, 90);
        g = add (67, 2.0, 0.5, 80);

        onOther = song.copyClip (clip, 8.0, true);
        phazite::moveClip (song.context, onOther, at (8.0), other);
        members = { clip, onOther };

        if (withSnippet)
        {
            snippet = song.copyClip (clip, 16.0, true);
            phazite::trimClipStart (song.context, snippet, at (17.0));
            members.push_back (snippet);
            REQUIRE (phazite::getClipRange (getSong(), snippet) == phazite::BeatRange (at (17.0), at (20.0)));
            REQUIRE (getContentStartOf (getSong(), snippet) == at (16.0));
            REQUIRE (phazite::getClipNotes (getSong(), snippet).size() == 3);
        }

        REQUIRE (sorted (phazite::getLinkedClips (song.context, clip)) == sorted (members));
        REQUIRE (phazite::getClipTrack (getSong(), onOther) == other);
        REQUIRE (phazite::getClipRange (getSong(), onOther) == phazite::BeatRange (at (8.0), at (12.0)));
        REQUIRE (haveTheSameNotes (getSong(), members));
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }

    NoteID add (int pitch, double beat, double length, int velocity)
    {
        const auto note = phazite::addNote (getSong(), clip, pitch, at (beat), lasting (length), velocity);
        REQUIRE (note.isValid());
        return note.note;
    }

    // A note as one member holds it (it must be there), and its start counted from that member's content start.
    NoteData noteIn (ClipID member, NoteID id) const
    {
        const auto data = phazite::getNoteData (getSong(), NoteRef { member, id });
        REQUIRE (data.has_value());
        return *data;
    }

    double startIn (ClipID member, NoteID id) const
    {
        return (noteIn (member, id).start - getContentStartOf (getSong(), member)).inBeats();
    }

    // A note through the second member, as the edits name it.
    NoteRef through (NoteID id) const { return { onOther, id }; }

    size_t countIn (ClipID member) const { return phazite::getClipNotes (getSong(), member).size(); }

    // Whether each member holds this many notes.
    bool eachHolds (size_t count) const
    {
        return std::all_of (members.begin(), members.end(), [this, count] (ClipID member) { return countIn (member) == count; });
    }

    // Each of a note's values, read through every member, with its start counted from that member's content start.
    void inEach (NoteID id, const std::function<void (const NoteData&, double start)>& expect) const
    {
        for (const auto member : members)
        {
            INFO ("in clip " << member.getRaw());
            expect (noteIn (member, id), startIn (member, id));
        }
    }

    // Where each member is in the song.
    std::string describePlaces() const
    {
        juce::StringArray places;

        for (const auto member : members)
        {
            const auto range = phazite::getClipRange (getSong(), member);
            places.add ("clip " + juce::String ((juce::int64) member.getRaw()) + " from " + describeBeats (range.getStart().inBeats())
                        + " to " + describeBeats (range.getEnd().inBeats()));
        }

        return places.joinIntoString ("\n").toStdString();
    }

    // An edit of a note named through two clips of its group, made with them named one way round, undone, then made with them
    // named the other way round and undone: each one undo step (UndoCheck), after which every member holds the same notes and
    // `verify` says what it made. Returns what the group was after each (every member's notes and where each is), and whether
    // `played` held after both.
    struct BothWays
    {
        std::string firstWay, otherWay;
        bool played = true;
    };

    BothWays editBothWays (phazite::test::UndoCheck& check, const juce::String& name,
                           const std::function<void (const std::vector<NoteRef>&)>& edit, const NoteRef& one, const NoteRef& two,
                           const std::function<void()>& verify, const std::function<bool()>& played)
    {
        auto& target = getSong();
        std::vector<std::string> after;
        BothWays result;

        for (const auto& named : { std::vector { one, two }, std::vector { two, one } })
        {
            INFO (name.toStdString() << ", named through clip " << named.front().clip.getRaw() << " first");
            check.check (name,
                         [&]
                         {
                             const phazite::UndoStep step (target, name);
                             edit (named);
                         });

            CHECK_MESSAGE (haveTheSameNotes (target, members), describeGroup (target, members));
            verify();
            result.played = result.played && played();
            after.push_back (describeGroup (target, members) + "\n" + describePlaces());
            check.stepBackOrForth ("Undo " + name, [&target] { phazite::undo (target); });
        }

        result.firstWay = after[0];
        result.otherWay = after[1];
        return result;
    }

    // An edit made through the model, checked: one undo step, exact (UndoCheck); what it made, as `verify` says, and every
    // member the same after it; Undo gives every member as it was before, the same, and Redo as it was after, the same, which
    // `verify` says again. With ownStep, the edit is a model function that is a step of its own; else it joins the step
    // begun for it, as a command's. The song is left as it was before the edit, for the next.
    void checkEdit (phazite::test::UndoCheck& check, const juce::String& name, const std::function<void()>& edit,
                    const std::function<void()>& verify, bool ownStep = false)
    {
        INFO (name.toStdString());
        auto& target = getSong();
        const auto before = describeGroup (target, members);
        REQUIRE (haveTheSameNotes (target, members));

        check.check (name,
                     [&]
                     {
                         if (ownStep)
                         {
                             edit();
                             return;
                         }

                         const phazite::UndoStep step (target, name);
                         edit();
                     });

        const auto after = describeGroup (target, members);
        CHECK (after != before);
        CHECK_MESSAGE (haveTheSameNotes (target, members), after);
        CHECK_MESSAGE (haveTheSameControls (target, members), after);
        verify();

        check.stepBackOrForth ("Undo " + name, [&target] { phazite::undo (target); });
        CHECK (describeGroup (target, members) == before);
        CHECK (haveTheSameNotes (target, members));

        check.stepBackOrForth ("Redo " + name, [&target] { phazite::redo (target); });
        CHECK (describeGroup (target, members) == after);
        CHECK (haveTheSameNotes (target, members));
        verify();

        check.stepBackOrForth ("Undo " + name + " again", [&target] { phazite::undo (target); });
        CHECK (describeGroup (target, members) == before);
    }

    phazite::test::TestSong song;
    phazite::TrackID other;
    ClipID clip, onOther, snippet;
    std::vector<ClipID> members;
    NoteID c, e, g;
};
} // namespace

TEST_CASE ("Every note edit made through an alias is made once, and every clip of its group holds the same notes after it, its Undo "
           "and its Redo")
{
    Aliased a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    const auto& members = a.members;

    // Each of a note's values, read through every member.
    const auto inEach = [&a, &members] (NoteID id, const std::function<void (const NoteData&, double start)>& expect)
    {
        for (const auto member : members)
        {
            INFO ("in clip " << member.getRaw());
            expect (a.noteIn (member, id), a.startIn (member, id));
        }
    };

    NoteRef added;
    a.checkEdit (
        check, "Add Note", [&] { added = phazite::addNote (song, a.onOther, 72, at (11.0), lasting (0.5), 70); },
        [&]
        {
            CHECK (added.clip == a.onOther);
            CHECK (a.eachHolds (4)); // one note, not one for each member
            inEach (added.note,
                    [] (const NoteData& note, double start)
                    {
                        CHECK (note.pitch == 72);
                        CHECK (start == doctest::Approx (3.0));
                        CHECK (note.velocity == 70);
                    });
        });

    a.checkEdit (
        check, "Move Note", [&] { phazite::moveNote (song, a.through (a.e), at (9.5), 65); },
        [&]
        {
            CHECK (a.eachHolds (3));
            inEach (a.e,
                    [] (const NoteData& note, double start)
                    {
                        CHECK (note.pitch == 65);
                        CHECK (start == doctest::Approx (1.5));
                    });
        });

    a.checkEdit (
        check, "Set Note Length", [&] { phazite::setNoteLength (song, a.through (a.e), lasting (2.0)); },
        [&] { inEach (a.e, [] (const NoteData& note, double) { CHECK (note.length.inBeats() == doctest::Approx (2.0)); }); });

    a.checkEdit (
        check, "Place Note", [&] { phazite::placeNote (song, a.through (a.g), at (10.5), 69, lasting (0.25)); },
        [&]
        {
            inEach (a.g,
                    [] (const NoteData& note, double start)
                    {
                        CHECK (note.pitch == 69);
                        CHECK (start == doctest::Approx (2.5));
                        CHECK (note.length.inBeats() == doctest::Approx (0.25));
                    });
        });

    a.checkEdit (
        check, "Place Notes",
        [&]
        {
            auto placed = phazite::getNoteData (song, std::vector { a.through (a.g), a.through (a.e) });
            REQUIRE (placed.size() == 2);
            placed[0].start = at (10.25);
            placed[0].pitch = 70;
            placed[0].length = lasting (0.5);
            placed[1].start = at (9.25);
            placed[1].pitch = 63;
            placed[1].length = lasting (0.75);
            phazite::placeNotes (song, placed);
        },
        [&]
        {
            inEach (a.g,
                    [] (const NoteData& note, double start)
                    {
                        CHECK (note.pitch == 70);
                        CHECK (start == doctest::Approx (2.25));
                        CHECK (note.length.inBeats() == doctest::Approx (0.5));
                    });
            inEach (a.e,
                    [] (const NoteData& note, double start)
                    {
                        CHECK (note.pitch == 63);
                        CHECK (start == doctest::Approx (1.25));
                        CHECK (note.length.inBeats() == doctest::Approx (0.75));
                    });
        });

    a.checkEdit (
        check, "Delete Note", [&] { phazite::deleteNotes (song, { a.through (a.g) }); },
        [&]
        {
            CHECK (a.eachHolds (2));

            for (const auto member : members)
                CHECK_FALSE (phazite::getNoteData (song, NoteRef { member, a.g }).has_value());
        });

    a.checkEdit (
        check, "Transpose Notes", [&] { phazite::transposeNotes (song, { a.through (a.c), a.through (a.e) }, 2); },
        [&]
        {
            inEach (a.c, [] (const NoteData& note, double) { CHECK (note.pitch == 62); });
            inEach (a.e, [] (const NoteData& note, double) { CHECK (note.pitch == 66); });
            inEach (a.g, [] (const NoteData& note, double) { CHECK (note.pitch == 67); });
        });

    a.checkEdit (
        check, "Nudge Notes", [&] { phazite::nudgeNotes (song, { a.through (a.e) }, lasting (0.25)); },
        [&] { inEach (a.e, [] (const NoteData&, double start) { CHECK (start == doctest::Approx (1.25)); }); });

    a.checkEdit (
        check, "Change Note Lengths", [&] { phazite::changeNoteLengths (song, { a.through (a.c), a.through (a.e) }, lasting (0.5)); },
        [&]
        {
            inEach (a.c, [] (const NoteData& note, double) { CHECK (note.length.inBeats() == doctest::Approx (1.5)); });
            inEach (a.e, [] (const NoteData& note, double) { CHECK (note.length.inBeats() == doctest::Approx (1.5)); });
        });

    a.checkEdit (
        check, "Mute Note", [&] { phazite::toggleNotesMuted (song, { a.through (a.e) }); },
        [&]
        {
            inEach (a.e, [] (const NoteData& note, double) { CHECK (note.muted); });
            inEach (a.c, [] (const NoteData& note, double) { CHECK_FALSE (note.muted); });
        });

    a.checkEdit (
        check, "Set Velocity", [&] { phazite::setNoteVelocities (song, { a.through (a.c), a.through (a.e) }, 50); },
        [&]
        {
            inEach (a.c, [] (const NoteData& note, double) { CHECK (note.velocity == 50); });
            inEach (a.e, [] (const NoteData& note, double) { CHECK (note.velocity == 50); });
            inEach (a.g, [] (const NoteData& note, double) { CHECK (note.velocity == 80); });
        });

    a.checkEdit (
        check, "Set Channel", [&] { phazite::setNoteChannels (song, { a.through (a.e) }, 3); },
        [&]
        {
            inEach (a.e, [] (const NoteData& note, double) { CHECK (note.channel == 3); });
            inEach (a.c, [] (const NoteData& note, double) { CHECK (note.channel == 1); });
        });

    a.checkEdit (
        check, "Slide Note", [&] { phazite::toggleNotesSlide (song, { a.through (a.g) }); },
        [&] { inEach (a.g, [] (const NoteData& note, double) { CHECK (note.slide); }); });

    a.checkEdit (
        check, "Portamento Note", [&] { phazite::toggleNotesPortamento (song, { a.through (a.g) }); },
        [&]
        {
            inEach (a.g,
                    [] (const NoteData& note, double)
                    {
                        CHECK (note.portamento);
                        CHECK_FALSE (note.slide);
                    });
        });

    a.checkEdit (
        check, "Change Velocity", [&] { phazite::changeNoteVelocities (song, { a.through (a.e) }, 5); },
        [&] { inEach (a.e, [] (const NoteData& note, double) { CHECK (note.velocity == 95); }); }, true);

    std::vector<NoteRef> copies;
    a.checkEdit (
        check, "Duplicate Notes", [&] { copies = phazite::duplicateNotes (song, { a.through (a.c), a.through (a.e) }); },
        [&]
        {
            REQUIRE (copies.size() == 2);
            CHECK (a.eachHolds (5));
            CHECK (copies[0].clip == a.onOther);
            inEach (copies[0].note,
                    [] (const NoteData& note, double start)
                    {
                        CHECK (note.pitch == 60);
                        CHECK (start == doctest::Approx (2.0));
                    });
            inEach (copies[1].note,
                    [] (const NoteData& note, double start)
                    {
                        CHECK (note.pitch == 64);
                        CHECK (start == doctest::Approx (3.0));
                    });
        });

    a.checkEdit (
        check, "Set Clip Notes",
        [&]
        {
            phazite::setClipNotes (
                song, a.onOther,
                { NoteData { .clip = a.onOther, .pitch = 48, .start = at (8.5), .length = lasting (1.0), .velocity = 64 },
                  NoteData { .clip = a.onOther, .pitch = 55, .start = at (10.0), .length = lasting (2.0), .velocity = 99 } });
        },
        [&]
        {
            CHECK (a.eachHolds (2));
            const auto notes = phazite::getClipNotes (song, a.onOther);
            REQUIRE (notes.size() == 2);
            CHECK (notes[0].pitch == 48);
            CHECK (notes[1].pitch == 55);
            CHECK (notes[0].id != notes[1].id);
            CHECK (a.startIn (a.clip, notes[0].id) == doctest::Approx (0.5));
            CHECK (a.startIn (a.snippet, notes[1].id) == doctest::Approx (2.0));
        });

    a.checkEdit (
        check, "Quantize Note",
        [&]
        {
            auto moved = a.noteIn (a.onOther, a.e);
            moved.start = at (9.25);
            const auto replaced = phazite::replaceNotes (song, { a.through (a.e) }, { moved });
            REQUIRE (replaced.size() == 1);
            CHECK (replaced[0] == a.through (a.e)); // its ID kept
        },
        [&]
        {
            CHECK (a.eachHolds (3));
            inEach (a.e, [] (const NoteData&, double start) { CHECK (start == doctest::Approx (1.25)); });
        });

    std::vector<NoteRef> pieces;
    a.checkEdit (
        check, "Chop Note", [&]
        { pieces = phazite::replaceNotes (song, { a.through (a.e) }, phazite::chop ({ a.noteIn (a.onOther, a.e) }, 0.25), "Chop Note"); },
        [&]
        {
            REQUIRE (pieces.size() == 4);
            CHECK (a.eachHolds (6));
            CHECK (pieces[0].note == a.e); // the first piece keeps the note's ID, in every member

            for (size_t i = 0; i < pieces.size(); ++i)
                inEach (pieces[i].note,
                        [i] (const NoteData& note, double start)
                        {
                            CHECK (note.pitch == 64);
                            CHECK (start == doctest::Approx (1.0 + 0.25 * (double) i));
                            CHECK (note.length.inBeats() == doctest::Approx (0.25));
                        });
        },
        true);

    CHECK (haveTheSameNotes (song, members));
    check.walkBackAndForth();
}

TEST_CASE ("Every controller edit made through an alias is made once, and every clip of its group holds the same values after it, its "
           "Undo and its Redo")
{
    Aliased a;
    auto& song = a.getSong();

    // Values where every member plays them (from beat 1 of its notes on, as the trimmed one does): the mod wheel at 2, 2.5 and 3,
    // the bend at 2.5.
    phazite::addControls (song, a.clip,
                          { { at (2.0), ControlKind::controller, 1, 40, 1 },
                            { at (2.5), ControlKind::controller, 1, 60, 1 },
                            { at (3.0), ControlKind::controller, 1, 80, 1 },
                            { at (2.5), ControlKind::pitchWheel, 0, 9000, 1 } });
    REQUIRE (haveTheSameControls (song, a.members));
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (a.song.context);

    // A lane's values that play, through each member, as beats from its content start and values.
    const auto valuesIn = [&song] (ClipID member, const ControlLane& lane)
    {
        std::vector<std::pair<double, int>> values;
        const auto from = getContentStartOf (song, member);

        for (const auto& point : phazite::getControlValues (song, { member }, lane))
            values.emplace_back ((point.beat - from).inBeats(), point.value);

        return values;
    };

    const auto eachHas = [&] (const ControlLane& lane, const std::vector<std::pair<double, int>>& expected)
    {
        for (const auto member : a.members)
        {
            INFO ("in clip " << member.getRaw());
            const auto values = valuesIn (member, lane);
            REQUIRE (values.size() == expected.size());

            for (size_t i = 0; i < values.size(); ++i)
            {
                CHECK (values[i].first == doctest::Approx (expected[i].first));
                CHECK (values[i].second == expected[i].second);
            }
        }
    };

    a.checkEdit (
        check, "Add Control", [&] { phazite::addControl (song, a.onOther, at (11.5), ControlKind::controller, 1, 100, 1); },
        [&] { eachHas (modWheel, { { 2.0, 40 }, { 2.5, 60 }, { 3.0, 80 }, { 3.5, 100 } }); });

    a.checkEdit (
        check, "Add Controls",
        [&]
        {
            phazite::addControls (song, a.onOther,
                                  { { at (10.25), ControlKind::controller, 1, 50, 2 },
                                    { at (11.0), ControlKind::controller, 1, 70, 2 },
                                    { at (11.0), ControlKind::pitchWheel, 0, 12000, 1 } });
        },
        [&]
        {
            eachHas (modWheelTwo, { { 2.25, 50 }, { 3.0, 70 } });
            eachHas (bend, { { 2.5, 9000 }, { 3.0, 12000 } });
            eachHas (modWheel, { { 2.0, 40 }, { 2.5, 60 }, { 3.0, 80 } }); // the first channel's as they were
        });

    a.checkEdit (
        check, "Remove Controls", [&] { phazite::removeControls (song, a.onOther, ControlKind::controller, 1, 1, at (10.25), at (11.0)); },
        [&]
        {
            eachHas (modWheel, { { 2.0, 40 } });
            eachHas (bend, { { 2.5, 9000 } });
        });

    a.checkEdit (
        check, "Remove Control", [&] { phazite::removeControl (song, a.onOther, { at (10.5), ControlKind::controller, 1, 60, 1 }); },
        [&] { eachHas (modWheel, { { 2.0, 40 }, { 3.0, 80 } }); });

    a.checkEdit (
        check, "Draw Mod Wheel",
        [&]
        {
            phazite::setControlValues (song, a.onOther, modWheel, { at (10.0), at (11.0) },
                                       { { {}, at (10.0), 10, 0 }, { {}, at (10.5), 20, 0 }, { {}, at (11.0), 30, 0 } });
        },
        [&] { eachHas (modWheel, { { 2.0, 10 }, { 2.5, 20 }, { 3.0, 30 } }); });

    a.checkEdit (
        check, "Delete Pitch Bend", [&] { phazite::deleteControlValues (song, { a.onOther }, bend, { at (8.0), at (12.0) }); },
        [&]
        {
            eachHas (bend, {});
            eachHas (modWheel, { { 2.0, 40 }, { 2.5, 60 }, { 3.0, 80 } });
        });

    a.checkEdit (
        check, "Delete All Mod Wheel", [&] { phazite::deleteAllControlValues (song, { a.onOther }, modWheel); },
        [&]
        {
            eachHas (modWheel, {});
            eachHas (bend, { { 2.5, 9000 } });
        });

    // Notes are left as they were by each.
    CHECK (haveTheSameNotes (song, a.members));
    CHECK (a.eachHolds (3));
    check.walkBackAndForth();
}

TEST_CASE ("A controller value before where a trimmed clip of its group starts is the group's, and edits through the trimmed clip "
           "leave it as it is")
{
    // Notes.h: "only the values that play are shown and changed ... a split or a trim leaves the others in the clip as they are.
    // Every change is made in each of the clip's aliases too." The snippet plays its notes from their beat 1: a value at their
    // beat 0.5 is one the first clip and the second play, and the snippet holds without playing (which no function of Notes.h
    // reads through it: it is read through the others).
    Aliased a;
    auto& song = a.getSong();
    phazite::addControls (song, a.clip,
                          { { at (0.5), ControlKind::controller, 1, 33, 1 }, { at (2.0), ControlKind::controller, 1, 44, 1 } });
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck check (a.song.context);

    // The mod wheel's values a member reads, as beats from its content start and values.
    const auto modWheelIn = [&song] (ClipID member)
    {
        juce::StringArray values;
        const auto from = getContentStartOf (song, member);

        for (const auto& point : phazite::getControlValues (song, { member }, modWheel))
            values.add (describeBeats ((point.beat - from).inBeats()) + "=" + juce::String (point.value));

        return values.joinIntoString (" ").toStdString();
    };

    REQUIRE (modWheelIn (a.clip) == "0.500000=33 2.000000=44");
    REQUIRE (modWheelIn (a.onOther) == "0.500000=33 2.000000=44");
    REQUIRE (modWheelIn (a.snippet) == "2.000000=44");
    REQUIRE (haveTheSameControls (song, a.members));

    // Deleted through the snippet, all of the lane it plays: the value before its start stays, in the clips that play it.
    a.checkEdit (
        check, "Delete All Mod Wheel", [&] { phazite::deleteAllControlValues (song, { a.snippet }, modWheel); },
        [&]
        {
            CHECK (modWheelIn (a.clip) == "0.500000=33");
            CHECK (modWheelIn (a.onOther) == "0.500000=33");
            CHECK (modWheelIn (a.snippet).empty());
        });

    // Drawn through the snippet over all of its notes: one at their beat 0.5, where it doesn't play, is left out, and the value
    // there stays as it was.
    a.checkEdit (
        check, "Draw Mod Wheel",
        [&]
        {
            phazite::setControlValues (song, a.snippet, modWheel, { at (16.0), at (20.0) },
                                       { { {}, at (16.5), 90, 0 }, { {}, at (18.0), 70, 0 } });
        },
        [&]
        {
            CHECK (modWheelIn (a.clip) == "0.500000=33 2.000000=70");
            CHECK (modWheelIn (a.onOther) == "0.500000=33 2.000000=70");
            CHECK (modWheelIn (a.snippet) == "2.000000=70");
        });

    a.checkEdit (
        check, "Delete Mod Wheel", [&] { phazite::deleteControlValues (song, { a.snippet }, modWheel, { at (16.0), at (20.0) }); },
        [&]
        {
            CHECK (modWheelIn (a.clip) == "0.500000=33");
            CHECK (modWheelIn (a.snippet).empty());
        });

    // Through the second, which plays it: gone from every clip of the group.
    a.checkEdit (
        check, "Delete All Mod Wheel", [&] { phazite::deleteAllControlValues (song, { a.onOther }, modWheel); },
        [&]
        {
            for (const auto member : a.members)
                CHECK (modWheelIn (member).empty());
        });

    check.walkBackAndForth();
    CHECK (modWheelIn (a.clip) == "0.500000=33 2.000000=44");
}

TEST_CASE ("A note's switches turned through an alias in a group of two clips are turned once")
{
    // In a group of three, a switch turned once for each clip reads as turned once; in a group of two, it would read as not
    // turned at all. Through the second clip, and named through both in one call.
    Aliased a (false);
    REQUIRE (a.members.size() == 2);
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    const auto both = [&a] (NoteID id) { return std::vector<NoteRef> { NoteRef { a.clip, id }, a.through (id) }; };

    for (const auto& named : { std::vector { a.through (a.e) }, both (a.e) })
    {
        INFO ("named through " << named.size() << " clips");
        a.checkEdit (
            check, "Mute Note", [&] { phazite::toggleNotesMuted (song, named); },
            [&]
            {
                a.inEach (a.e, [] (const NoteData& note, double) { CHECK (note.muted); });
                a.inEach (a.c, [] (const NoteData& note, double) { CHECK_FALSE (note.muted); });
            });

        a.checkEdit (
            check, "Slide Note", [&] { phazite::toggleNotesSlide (song, named); },
            [&] { a.inEach (a.e, [] (const NoteData& note, double) { CHECK (note.slide); }); });

        a.checkEdit (
            check, "Portamento Note", [&] { phazite::toggleNotesPortamento (song, named); },
            [&] { a.inEach (a.e, [] (const NoteData& note, double) { CHECK (note.portamento); }); });
    }

    check.walkBackAndForth();
}

TEST_CASE ("A note named through two clips of its group in one call is edited once")
{
    // A note selected in two clips the piano roll shows, of one alias group, is one note: Notes.cpp's withoutAliasCopies()
    // edits it once. Transposed by 1, it moves 1; nudged, it moves once; one copy is made; deleted, no copy of it stays.
    Aliased a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    const auto both = [&a] (NoteID id) { return std::vector<NoteRef> { NoteRef { a.clip, id }, a.through (id) }; };
    const auto inEach = [&a] (NoteID id, const std::function<void (const NoteData&, double start)>& expect)
    {
        for (const auto member : a.members)
        {
            INFO ("in clip " << member.getRaw());
            expect (a.noteIn (member, id), a.startIn (member, id));
        }
    };

    a.checkEdit (
        check, "Transpose Note", [&] { phazite::transposeNotes (song, both (a.c), 1); },
        [&] { inEach (a.c, [] (const NoteData& note, double) { CHECK (note.pitch == 61); }); });

    a.checkEdit (
        check, "Nudge Note", [&] { phazite::nudgeNotes (song, both (a.e), lasting (0.25)); },
        [&] { inEach (a.e, [] (const NoteData&, double start) { CHECK (start == doctest::Approx (1.25)); }); });

    a.checkEdit (
        check, "Lengthen Note", [&] { phazite::changeNoteLengths (song, both (a.e), lasting (0.5)); },
        [&] { inEach (a.e, [] (const NoteData& note, double) { CHECK (note.length.inBeats() == doctest::Approx (1.5)); }); });

    a.checkEdit (
        check, "Change Velocity", [&] { phazite::changeNoteVelocities (song, both (a.e), 10); },
        [&] { inEach (a.e, [] (const NoteData& note, double) { CHECK (note.velocity == 100); }); }, true);

    a.checkEdit (
        check, "Mute Note", [&] { phazite::toggleNotesMuted (song, both (a.e)); },
        [&] { inEach (a.e, [] (const NoteData& note, double) { CHECK (note.muted); }); });

    a.checkEdit (
        check, "Slide Note", [&] { phazite::toggleNotesSlide (song, both (a.g)); },
        [&] { inEach (a.g, [] (const NoteData& note, double) { CHECK (note.slide); }); });

    a.checkEdit (
        check, "Portamento Note", [&] { phazite::toggleNotesPortamento (song, both (a.g)); },
        [&] { inEach (a.g, [] (const NoteData& note, double) { CHECK (note.portamento); }); });

    a.checkEdit (
        check, "Set Velocity", [&] { phazite::setNoteVelocities (song, both (a.e), 50); },
        [&]
        {
            inEach (a.e, [] (const NoteData& note, double) { CHECK (note.velocity == 50); });
            inEach (a.c, [] (const NoteData& note, double) { CHECK (note.velocity == 100); });
        });

    a.checkEdit (
        check, "Set Channel", [&] { phazite::setNoteChannels (song, both (a.e), 5); },
        [&] { inEach (a.e, [] (const NoteData& note, double) { CHECK (note.channel == 5); }); });

    // A drag of a quarter beat later and a semitone up, as the piano roll gives it for each clip's copy of the note.
    a.checkEdit (
        check, "Place Notes",
        [&]
        {
            auto placed = phazite::getNoteData (song, both (a.g));
            REQUIRE (placed.size() == 2);

            for (auto& note : placed)
            {
                note.start = note.start + lasting (0.25);
                note.pitch = 68;
            }

            phazite::placeNotes (song, placed);
        },
        [&]
        {
            CHECK (a.eachHolds (3));
            inEach (a.g,
                    [] (const NoteData& note, double start)
                    {
                        CHECK (note.pitch == 68);
                        CHECK (start == doctest::Approx (2.25));
                        CHECK (note.length.inBeats() == doctest::Approx (0.5));
                    });
        });

    // A controller's value added through the second: one in each clip, not one for each.
    a.checkEdit (
        check, "Add Control", [&] { phazite::addControl (song, a.onOther, at (11.0), ControlKind::controller, 1, 64, 1); },
        [&]
        {
            for (const auto member : a.members)
            {
                INFO ("in clip " << member.getRaw());
                const auto values = phazite::getControlValues (song, { member }, modWheel);
                REQUIRE (values.size() == 1);
                CHECK ((values[0].beat - getContentStartOf (song, member)).inBeats() == doctest::Approx (3.0));
                CHECK (values[0].value == 64);
            }
        });

    std::vector<NoteRef> copies;
    a.checkEdit (
        check, "Duplicate Note", [&] { copies = phazite::duplicateNotes (song, both (a.e)); },
        [&]
        {
            REQUIRE (copies.size() == 1);
            CHECK (a.eachHolds (4));
            inEach (copies[0].note, [] (const NoteData&, double start) { CHECK (start == doctest::Approx (2.0)); });
        });

    a.checkEdit (
        check, "Delete Note", [&] { phazite::deleteNotes (song, both (a.e)); },
        [&]
        {
            CHECK (a.eachHolds (2));

            for (const auto member : a.members)
                CHECK_FALSE (phazite::getNoteData (song, NoteRef { member, a.e }).has_value());
        });

    // The same through the selection, as the piano roll showing both clips selects a note in each, and the commands.
    auto& selection = getSelection (a.song.context);
    phazite::CommandRegistry noteCommands;
    phazite::addNoteCommands (noteCommands, selection, [] { return 0.25; });
    selection.setNoteClips ({ a.clip, a.onOther });
    selection.selectNotes (both (a.c));
    REQUIRE (phazite::getSelectedNotes (selection).size() == 2);

    check.command (noteCommands, "note.transposeUp");
    inEach (a.c, [] (const NoteData& note, double) { CHECK (note.pitch == 61); });
    check.command (noteCommands, "note.octaveDown");
    inEach (a.c, [] (const NoteData& note, double) { CHECK (note.pitch == 49); });

    // Replaced through the selection, as Legato and Chop replace the selected notes (replaceSelectedNotes()): each note of the
    // group once afterwards. Legato of c and g: c lasts until g starts.
    selection.selectNotes ({ NoteRef { a.clip, a.c }, a.through (a.c), NoteRef { a.clip, a.g }, a.through (a.g) });
    check.command (noteCommands, "note.legato");
    CHECK (a.eachHolds (3));
    CHECK_MESSAGE (haveTheSameNotes (song, a.members), describeGroup (song, a.members));
    inEach (a.c, [] (const NoteData& note, double) { CHECK (note.length.inBeats() == doctest::Approx (2.0)); });
    inEach (a.g, [] (const NoteData&, double start) { CHECK (start == doctest::Approx (2.0)); });

    // Chop of e on the grid of a quarter beat: four pieces of it, once.
    selection.selectNotes (both (a.e));
    check.command (noteCommands, "note.chop");
    CHECK (a.eachHolds (6));
    CHECK_MESSAGE (haveTheSameNotes (song, a.members), describeGroup (song, a.members));
    CHECK (phazite::getSelectedNotes (selection).size() == 4);
    inEach (a.e,
            [] (const NoteData& note, double start)
            {
                CHECK (start == doctest::Approx (1.0));
                CHECK (note.length.inBeats() == doctest::Approx (0.25));
            });

    selection.selectNotes (both (a.c));
    check.command (a.song.commands, "edit.duplicate");
    CHECK (a.eachHolds (7));
    CHECK (haveTheSameNotes (song, a.members));

    selection.selectNotes (both (a.c));
    check.command (a.song.commands, "edit.delete");
    CHECK (a.eachHolds (6));
    CHECK_FALSE (phazite::getNoteData (song, NoteRef { a.snippet, a.c }).has_value());
    CHECK (haveTheSameNotes (song, a.members));

    selection.deselectAll();
    check.walkBackAndForth();
}

// Edits of a note named through two clips of its group in one call, named one way round and the other: the first clip, whose
// notes play from their beat 0, and the snippet, whose notes play from their beat 1. g starts at their beat 2.
TEST_CASE ("A nudge of a note named through two clips of its group stops where either clip would stop playing it, whichever is "
           "named first")
{
    // Notes.h, nudgeNotes(): "none to before its clip's start or to where its clip doesn't play it, as moveNote() says"; and
    // Notes.cpp, keepPlayed(): "An edit of a note that plays never leaves it in its clip without playing". Back by a beat and a
    // half: through the first clip, g could go to its beat 0.5; through the snippet, no further than 1, where it starts.
    Aliased a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    const NoteRef inClip { a.clip, a.g }, inSnippet { a.snippet, a.g };
    REQUIRE (phazite::isNotePlayed (song, inSnippet));

    const auto result = a.editBothWays (
        check, "Nudge Notes", [&] (const std::vector<NoteRef>& named) { phazite::nudgeNotes (song, named, lasting (-1.5)); }, inClip,
        inSnippet, [&] { a.inEach (a.g, [] (const NoteData&, double start) { CHECK (start == doctest::Approx (1.0)); }); },
        [&] { return phazite::isNotePlayed (song, inClip) && phazite::isNotePlayed (song, inSnippet); });

    CHECK (result.firstWay == result.otherWay);
    CHECK (result.played);
    check.walkBackAndForth();
}

TEST_CASE ("A transpose of a note named through two clips of its group moves it once, whichever is named first")
{
    // Notes.h, transposeNotes(): "all by the same amount and none beyond the MIDI range"; the range is the note's, the same in
    // every clip of its group.
    Aliased a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    const NoteRef inClip { a.clip, a.g }, inSnippet { a.snippet, a.g };

    const auto result = a.editBothWays (
        check, "Transpose Notes", [&] (const std::vector<NoteRef>& named) { phazite::transposeNotes (song, named, 3); }, inClip, inSnippet,
        [&] { a.inEach (a.g, [] (const NoteData& note, double) { CHECK (note.pitch == 70); }); },
        [&] { return phazite::isNotePlayed (song, inClip) && phazite::isNotePlayed (song, inSnippet); });

    CHECK (result.firstWay == result.otherWay);
    CHECK (result.played);
    check.walkBackAndForth();
}

// FINDING (known bug 231): placeNotes() places each note as named, the same note through each clip of its group in turn, and the last named
// takes it to where the snippet, trimmed to start later, no longer plays it.
TEST_CASE ("A drag of a note named through two clips of its group stops where either clip would stop playing it" * doctest::should_fail())
{
    // Notes.h, placeNotes(): each "placed as placeNote() says", which moves it "as moveNote() and setNoteLength() say": "a move
    // never takes a note that plays to where its clip doesn't play it". A drag back by a beat and a half, as the piano roll gives
    // it for each clip's copy of g: through the snippet, g can go no further back than its beat 1.
    Aliased a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    const NoteRef inClip { a.clip, a.g }, inSnippet { a.snippet, a.g };

    const auto result = a.editBothWays (
        check, "Place Notes",
        [&] (const std::vector<NoteRef>& named)
        {
            auto placed = phazite::getNoteData (song, named);
            REQUIRE (placed.size() == 2);

            for (auto& note : placed)
                note.start = note.start - lasting (1.5);

            phazite::placeNotes (song, placed);
        },
        inClip, inSnippet, [] {}, [&] { return phazite::isNotePlayed (song, inClip) && phazite::isNotePlayed (song, inSnippet); });

    REQUIRE (result.firstWay == result.otherWay); // the same either way round
    CHECK (result.played);
    check.walkBackAndForth();
}

// FINDING (known bug 232): changeNoteLengths() lengthens a note named through two clips of its group once, through the clip named first, and
// only that clip grows to hold it: which clip grows depends on the order the clips are named in.
TEST_CASE ("A note named through two clips of its group lengthened past their ends gives the same song whichever is named first"
           * doctest::should_fail())
{
    // Notes.h, changeNoteLengths(): the notes longer "by the same amount", and setNoteLength(): "the clip grows to hold it, as
    // addNote() says". Lengthened by two beats, g ends at its beat 4.5, past the end of both clips named: the first clip has room
    // to grow up to the snippet, the snippet as far as it likes.
    Aliased a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    const NoteRef inClip { a.clip, a.g }, inSnippet { a.snippet, a.g };

    const auto result = a.editBothWays (
        check, "Lengthen Notes", [&] (const std::vector<NoteRef>& named) { phazite::changeNoteLengths (song, named, lasting (2.0)); },
        inClip, inSnippet,
        [&] { a.inEach (a.g, [] (const NoteData& note, double) { CHECK (note.length.inBeats() == doctest::Approx (2.5)); }); },
        [&] { return phazite::isNotePlayed (song, inClip) && phazite::isNotePlayed (song, inSnippet); });

    REQUIRE (result.played);
    CHECK_MESSAGE (result.firstWay == result.otherWay, result.firstWay << "\n----\n" << result.otherWay);
    check.walkBackAndForth();
}

TEST_CASE ("replaceNotes() given a note through two clips of its group adds it twice: naming each note once is its caller's job")
{
    // Notes.h, replaceNotes(): the old ones go and the replacements are added, one with an old one's ID keeping it, "the others
    // that come of an old one" (a chopped note's later pieces) added beside it. It doesn't take a note named twice as one: its
    // callers name each note once, as replaceSelectedNotes() does (Notes.cpp: withoutAliasCopies()). Pinned as it is today.
    Aliased a;
    auto& song = a.getSong();
    phazite::test::UndoCheck check (a.song.context);
    const std::vector named { NoteRef { a.clip, a.e }, a.through (a.e) };
    std::vector<NoteRef> made;

    a.checkEdit (
        check, "Quantize Note",
        [&]
        {
            auto moved = phazite::getNoteData (song, named);
            REQUIRE (moved.size() == 2);

            for (auto& note : moved)
                note.start = note.start + lasting (0.25);

            made = phazite::replaceNotes (song, named, moved);
        },
        [&]
        {
            REQUIRE (made.size() == 2);
            CHECK (made[0] == named[0]); // its ID kept
            CHECK (made[1].clip == a.onOther);
            CHECK (made[1].note != a.e); // a note of its own: the group holds it twice
            CHECK (a.eachHolds (4));
            a.inEach (a.e, [] (const NoteData&, double start) { CHECK (start == doctest::Approx (1.25)); });
            a.inEach (made[1].note,
                      [] (const NoteData& note, double start)
                      {
                          CHECK (note.pitch == 64);
                          CHECK (start == doctest::Approx (1.25));
                      });
        });

    check.walkBackAndForth();
}

// FINDING (known bug 233): the piano roll's Quantize (PianoRoll::quantizeSelection()) hands replaceNotes() a note selected in two clips of its
// group once for each clip, and the group then holds the note twice.
TEST_CASE ("Quantize of a note selected in two clips of its group the piano roll shows moves it once" * doctest::should_fail())
{
    // Notes.h: each edit "is made to the note in each of its clip's aliases too"; the selection's own replace
    // (replaceSelectedNotes(), Legato and Chop) names each note once. A clip from beat 0 to 8 with a note at 1.25, its alias on
    // the second track from beat 8, both shown, the note selected in each; the grid a beat.
    phazite::test::PianoRollScreen screen;
    auto& song = screen.getSong();
    const auto note = phazite::addNote (song, screen.clip, 60, at (1.25), lasting (0.5), 100);
    REQUIRE (note.isValid());
    const auto aliases = phazite::copyClips (song, { screen.clip }, true);
    REQUIRE (aliases.size() == 1);
    const auto alias = aliases.front();
    phazite::moveClip (song, alias, at (8.0), screen.tracks[1]);
    song.getUndoManager().clearUndoHistory();
    screen.pianoRoll.showClips ({ screen.clip, alias });
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.pianoRoll.getClips().size() == 2);
    getSelection (screen.context).selectNotes ({ note, NoteRef { alias, note.note } });
    phazite::test::dispatchPendingMessages();

    phazite::test::UndoCheck check (screen.context);
    check.command (screen.registry, "edit.quantize");
    REQUIRE (haveTheSameNotes (song, { screen.clip, alias }));
    REQUIRE (phazite::getNoteData (song, note).has_value());
    REQUIRE (phazite::getNoteData (song, note)->start.inBeats() == doctest::Approx (1.0));
    CHECK (phazite::getClipNotes (song, screen.clip).size() == 1);
    check.walkBackAndForth();
}

TEST_CASE ("Notes pasted and duplicated into a clip of a group reach every clip of it, and a replaced note keeps its ID in each")
{
    Aliased a;
    auto& song = a.getSong();
    auto& selection = getSelection (a.song.context);
    phazite::test::UndoCheck check (a.song.context);

    // c and e copied from the first clip, pasted at beat 10 into the second, which the piano roll shows: at its beats 2 and 3.
    selection.setNoteClips ({ a.clip });
    selection.selectNotes ({ NoteRef { a.clip, a.c }, NoteRef { a.clip, a.e } });
    a.song.run ("edit.copy");
    selection.setNoteClips ({ a.onOther });
    phazite::movePlayhead (song, at (10.0));
    check.command (a.song.commands, "edit.paste");
    CHECK (a.eachHolds (5));
    CHECK_MESSAGE (haveTheSameNotes (song, a.members), describeGroup (song, a.members));

    const auto pasted = phazite::getSelectedNotes (selection);
    REQUIRE (pasted.size() == 2);

    for (const auto& note : pasted)
    {
        CHECK (note.clip == a.onOther);
        CHECK (note.note != a.c);
        CHECK (note.note != a.e);

        for (const auto member : a.members)
            CHECK (phazite::getNoteData (song, NoteRef { member, note.note }).has_value());
    }

    CHECK (a.startIn (a.snippet, pasted[0].note) == doctest::Approx (2.0));
    CHECK (a.startIn (a.clip, pasted[1].note) == doctest::Approx (3.0));

    // Duplicated in the second: one copy, in each.
    selection.selectNotes ({ a.through (a.g) });
    check.command (a.song.commands, "edit.duplicate");
    CHECK (a.eachHolds (6));
    CHECK (haveTheSameNotes (song, a.members));
    const auto duplicated = phazite::getSelectedNotes (selection);
    REQUIRE (duplicated.size() == 1);
    CHECK (a.startIn (a.clip, duplicated[0].note) == doctest::Approx (2.5));

    // Replaced through the second, as a quantize replaces it: the same ID in every clip of the group, where it was put.
    selection.deselectAll();
    check.check ("Quantize Notes",
                 [&]
                 {
                     auto quantized = phazite::getNoteData (song, std::vector { a.through (a.e), a.through (a.g) });
                     REQUIRE (quantized.size() == 2);
                     quantized[0].start = at (9.25);
                     quantized[1].start = at (10.5);
                     const auto replaced = phazite::replaceNotes (song, { a.through (a.e), a.through (a.g) }, quantized, "Quantize Notes");
                     REQUIRE (replaced.size() == 2);
                     CHECK (replaced[0] == a.through (a.e));
                     CHECK (replaced[1] == a.through (a.g));
                 });

    CHECK (a.eachHolds (6));
    CHECK (haveTheSameNotes (song, a.members));

    for (const auto member : a.members)
    {
        CHECK (a.startIn (member, a.e) == doctest::Approx (1.25));
        CHECK (a.startIn (member, a.g) == doctest::Approx (2.5));
    }

    check.walkBackAndForth();
    CHECK (haveTheSameNotes (song, a.members));
}
