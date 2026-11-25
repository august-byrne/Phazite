// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's own change hears it from one place (model/base/internal/SongValues.h: Listener::songOwnChanged(),
// songStandingChanged(), songOutsideChanged()), each value once for each change of it (roadmap step S5.20, the song's own:
// its three areas hold it beside the song's tree, and each tells the changes of its own value: model/base/internal/
// SongOwnArea.h). Each writer of the song's own, through its setter (SongOwnValue.h), tells its value once with what differs,
// makes the undo step it always made or none, and its Undo and its Redo tell once each; what is written outside the history
// survives the Undo of a step that changed another entry of the song's standing; what plays makes a plan for the loop and the
// metronome alone; the unsaved check counts what is counted and nothing of how the song plays and shows; the versions' pass
// and a merge's choice set the song's own through the setters. Written to hold unchanged across the switch of the storage.
//   phazite_tests.exe --test-case="Song values' own*"
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestSongOwn.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/GridSettings.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnArea.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/NoteValues.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/VersionHistory.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace
{
using phazite::SongOwnChange;
namespace schema = phazite::schema;

phazite::BeatPosition at (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

juce::String repeat (char c, int times)
{
    return juce::String::repeatedString (juce::String::charToString ((juce::juce_wchar) c), times);
}

// What the values' listener was told of the song's own: how many calls of each value, the parts of all the song's own calls,
// whether the key and the names were told, and whether what plays was.
struct Told final : phazite::SongValues::Listener
{
    int own = 0, standing = 0, outside = 0;
    std::uint8_t parts = 0;
    bool lastKey = false, names = false, plays = false;

    void songOwnChanged (const SongOwnChange& change) override
    {
        CHECK (change.parts != 0); // told only where something differs
        ++own;
        parts = (std::uint8_t) (parts | change.parts);
    }

    void songStandingChanged (bool key, bool versionsNames) override
    {
        CHECK ((key || versionsNames));
        ++standing;
        lastKey = lastKey || key;
        names = names || versionsNames;
    }

    void songOutsideChanged (bool whatPlays) override
    {
        ++outside;
        plays = plays || whatPlays;
    }
};

// A song of a project, its values' listener heard, and the steps its history holds.
struct Watched
{
    Watched() { song.getValues()->addListener (&told); }
    ~Watched() { song.getValues()->removeListener (&told); }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    Told told {};

    int countSteps() const { return song.getUndoManager().getUndoDescriptions().size(); }

    // What a change told, and how many steps it added to the history.
    struct Heard
    {
        Told told {};
        int steps = 0;
    };

    Heard hear (const std::function<void()>& change)
    {
        told = {};
        const auto before = countSteps();
        change();
        phazite::test::checkSong (song); // the live root as its values write it, and the rest of the song whole
        return { told, countSteps() - before };
    }

    Heard undo()
    {
        return hear ([this] { phazite::undo (song); });
    }

    Heard redo()
    {
        return hear ([this] { phazite::redo (song); });
    }

    std::function<void()> inStep (const juce::String& name, const std::function<void()>& change)
    {
        return [this, name, change]
        {
            const phazite::UndoStep step (song, name);
            change();
        };
    }
};

void checkOwnOnce (const Watched::Heard& heard, std::uint8_t parts)
{
    CHECK (heard.told.own == 1);
    CHECK (heard.told.parts == parts);
    CHECK (heard.told.standing == 0);
    CHECK (heard.told.outside == 0);
}

void checkOutsideOnce (const Watched::Heard& heard, bool plays)
{
    CHECK (heard.told.outside == 1);
    CHECK (heard.told.plays == plays);
    CHECK (heard.told.own == 0);
    CHECK (heard.told.standing == 0);
    CHECK (heard.steps == 0);
}

phazite::GridSettings withSnap (phazite::Song& song, phazite::GridOwner owner)
{
    auto grid = phazite::loadGridSettings (song, owner);
    grid.snap = ! grid.snap;
    grid.fixedLevel = phazite::GridSettings::minFixedLevel + 2;
    return grid;
}
} // namespace

//==============================================================================
TEST_CASE ("Song values' own: each writer in the history tells its value once with what changed, in one step, and its Undo and "
           "Redo once each")
{
    Watched w;

    struct Edit
    {
        const char* what = "";
        std::function<void()> make {};
        std::uint8_t parts = 0;
    };

    const std::vector<Edit> edits {
        { "an outline applied", w.inStep ("Outline", [&] { phazite::test::setStepProbe (w.song, "verse-chorus"); }),
          SongOwnChange::outline },
        { "the project's notes", [&] { phazite::setProjectNotes (w.s.context, "Bridge in D\nmore bass"); }, SongOwnChange::notes },
        { "the notes again", [&] { phazite::setProjectNotes (w.s.context, "Bridge in E"); }, SongOwnChange::notes },
        { "the notes taken away", [&] { phazite::setProjectNotes (w.s.context, {}); }, SongOwnChange::notes },
        { "patterns keep their speed", w.inStep ("Patterns", [&] { phazite::togglePatternsFollowTempo (w.song); }),
          SongOwnChange::patterns },
        { "patterns follow again", w.inStep ("Patterns", [&] { phazite::togglePatternsFollowTempo (w.song); }), SongOwnChange::patterns },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        const auto made = w.hear (edit.make);
        checkOwnOnce (made, edit.parts);
        CHECK (made.steps == 1);

        checkOwnOnce (w.undo(), edit.parts);
        checkOwnOnce (w.redo(), edit.parts);
    }

    // The same set again: no change, no step, nothing told.
    const auto again = w.hear ([&] { phazite::setProjectNotes (w.s.context, {}); });
    CHECK (again.told.own == 0);
    CHECK (again.steps == 0);
}

TEST_CASE ("Song values' own: what the song keeps of how it plays and shows is set outside the history, each set told once, what "
           "plays among it said")
{
    Watched w;

    struct Edit
    {
        const char* what = "";
        std::function<void()> make {};
        bool plays = false;
    };

    // In the order they are made, each on the song as the ones before left it. A loop's range writes four properties, a grid
    // five: told once each.
    const std::vector<Edit> edits {
        { "the loop on", [&] { phazite::toggleLoop (w.song); }, true },
        { "a range looped", [&] { phazite::loopBeats (w.song, { at (4.0), at (12.0) }); }, true },
        { "the song looped", [&] { phazite::loopSong (w.song); }, true },
        { "the loop off", [&] { phazite::toggleLoop (w.song); }, true },
        { "the metronome on", [&] { phazite::setMetronomeOn (w.song, true); }, true },
        { "the play start moved", [&] { phazite::setPlayStart (w.song, at (8.0)); }, false },
        { "the play start at the song's start", [&] { phazite::setPlayStart (w.song, at (0.0)); }, false },
        { "punch on", [&] { phazite::setPunchOn (w.song, true); }, false },
        { "the playlist's grid", [&]
          { phazite::saveGridSettings (w.song, phazite::GridOwner::playlist, withSnap (w.song, phazite::GridOwner::playlist)); }, false },
        { "the piano roll's grid, its first", [&]
          { phazite::saveGridSettings (w.song, phazite::GridOwner::pianoRoll, withSnap (w.song, phazite::GridOwner::pianoRoll)); }, false },
        { "the piano roll's grid again", [&]
          { phazite::saveGridSettings (w.song, phazite::GridOwner::pianoRoll, withSnap (w.song, phazite::GridOwner::pianoRoll)); }, false },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        checkOutsideOnce (w.hear (edit.make), edit.plays);
    }

    // Inside a step another writer opened: still outside it, the step holding nothing of it.
    const auto inStep = w.hear (w.inStep ("Something", [&] { phazite::setMetronomeOn (w.song, false); }));
    checkOutsideOnce (inStep, true);

    // The same again: nothing told.
    CHECK (w.hear ([&] { phazite::setPunchOn (w.song, true); }).told.outside == 0);

    // What was set, as the readers read it.
    CHECK_FALSE (phazite::isLoopOn (w.song));
    CHECK (phazite::isLoopingSong (w.song) == false);
    CHECK (phazite::isPunchOn (w.song));
    CHECK (phazite::getPlayStartBeat (w.song) == at (0.0));
}

TEST_CASE ("Song values' own: a range looped past the song's most beats is held within them")
{
    Watched w;
    phazite::loopBeats (w.song, { at (schema::maxBeats - 8.0), at (schema::maxBeats + 100.0) });
    const auto outside = phazite::getSongOutside (w.song);
    REQUIRE (outside.loopEnd.has_value());
    CHECK (*outside.loopEnd == doctest::Approx (schema::maxBeats));
    CHECK (phazite::getLoopBeats (w.song).getEnd() == at (schema::maxBeats));
    phazite::test::checkSong (w.song);

    // One that the bound leaves no range of loops nothing new.
    const auto before = phazite::getSongOutside (w.song);
    phazite::loopBeats (w.song, { at (schema::maxBeats + 1.0), at (schema::maxBeats + 2.0) });
    CHECK (phazite::getSongOutside (w.song) == before);
    phazite::test::checkSong (w.song);
}

TEST_CASE ("Song values' own: the key set last and the versions' names are written outside the history, told as the standing")
{
    Watched w;

    // A merge waiting set by hand, as a file from anyone may hold it: the song's own value, outside the history,
    // first, in a song with nothing to undo yet (VersionHistory.h: setWaitingMerge()).
    phazite::MergingRecord merge;
    merge.version = repeat ('a', 64);
    merge.head = repeat ('b', 64);
    merge.before = repeat ('c', 64);
    const auto waiting = w.hear ([&] { phazite::setWaitingMerge (w.song, phazite::writeMergingElement (merge)); });
    checkOwnOnce (waiting, SongOwnChange::merging);
    CHECK (waiting.steps == 0);
    CHECK (phazite::test::hasWaitingMerge (w.song));

    const auto gone = w.hear ([&] { phazite::setWaitingMerge (w.song, {}); });
    checkOwnOnce (gone, SongOwnChange::merging);
    CHECK_FALSE (phazite::test::hasWaitingMerge (w.song));

    // A key set by itself (a new project's content, the key menu's last): told, no step.
    const auto set = w.hear ([&] { phazite::setLastKey (w.song, { .root = 2, .scale = phazite::Scale::minor }); });
    CHECK (set.told.standing == 1);
    CHECK (set.told.lastKey);
    CHECK_FALSE (set.told.names);
    CHECK (set.steps == 0);
    CHECK (phazite::getLastKey (w.song) == phazite::Key { .root = 2, .scale = phazite::Scale::minor });

    // A track's key chosen: the track's in the step, the key set last outside it; its Undo puts the track's back alone.
    const auto chosen = w.hear ([&] { phazite::chooseTrackKey (w.song, w.s.firstTrack, phazite::Key { .root = 7 }); });
    CHECK (chosen.told.standing == 1);
    CHECK (chosen.told.lastKey);
    CHECK (chosen.steps == 1);

    const auto undone = w.undo();
    CHECK (undone.told.standing == 0);
    CHECK (phazite::getLastKey (w.song) == phazite::Key { .root = 7 });
}

//==============================================================================
TEST_CASE ("Song values' own: a version applied that differs in every counted part is one step, told once, its Undo exact, and a "
           "second pass writes nothing")
{
    Watched w;
    phazite::test::setStepProbe (w.song, "mine");
    phazite::setLastKey (w.song, { .root = 1 });
    w.song.getUndoManager().clearUndoHistory();

    auto other = phazite::test::copyWholeSong (w.song);
    other.setProperty (schema::outline, "theirs", nullptr);
    other.setProperty (schema::notes, "Their notes", nullptr);
    other.setProperty (schema::patternsKeepTheirSpeed, true, nullptr);
    other.setProperty (schema::idNumbering, repeat ('e', schema::idNumberingDigits), nullptr);
    juce::ValueTree (other.getChildWithName (schema::LASTKEY)).setProperty (schema::keyRoot, 9, nullptr);

    phazite::test::UndoCheck undo (w.song);
    Watched::Heard applied;
    undo.check ("Apply", [&] { applied = w.hear (w.inStep ("Apply", [&] { phazite::makeSongLike (w.song, other); })); });

    CHECK (applied.steps == 1);
    CHECK (applied.told.own == 1);
    CHECK (applied.told.parts == (SongOwnChange::outline | SongOwnChange::notes | SongOwnChange::idNumbering | SongOwnChange::patterns));
    CHECK (applied.told.standing == 1);
    CHECK (applied.told.lastKey);

    const auto own = phazite::getSongOwn (w.song);
    CHECK (own.outline == "theirs");
    CHECK (own.notes == "Their notes");
    CHECK (own.patternsKeepTheirSpeed);
    CHECK (own.idNumbering == repeat ('e', schema::idNumberingDigits));
    CHECK (phazite::getLastKey (w.song)->root == 9);

    // Its Undo and its Redo, each told once (the check above undid and redid it, and left it done).
    const auto undone = w.undo();
    CHECK (undone.told.own == 1);
    CHECK (undone.told.standing == 1);
    CHECK (phazite::getSongOwn (w.song).outline == "mine");
    CHECK (phazite::getLastKey (w.song)->root == 1);
    const auto redone = w.redo();
    CHECK (redone.told.own == 1);
    CHECK (redone.told.standing == 1);

    // Again: nothing written, no step, nothing told.
    const auto again = w.hear (w.inStep ("Apply", [&] { phazite::makeSongLike (w.song, other); }));
    CHECK (again.steps == 0);
    CHECK (again.told.own == 0);
    CHECK (again.told.standing == 0);
}

TEST_CASE ("Song values' own: a key set outside the history survives the Undo of a version applied that changed the song's own")
{
    Watched w;
    phazite::test::setStepProbe (w.song, "mine");
    auto other = phazite::test::copyWholeSong (w.song);
    other.setProperty (schema::outline, "theirs", nullptr);

    {
        const phazite::UndoStep step (w.song, "Apply");
        phazite::makeSongLike (w.song, other);
    }

    phazite::test::checkSong (w.song);
    phazite::setLastKey (w.song, { .root = 4 });
    phazite::test::checkSong (w.song);
    phazite::undo (w.song);
    phazite::test::checkSong (w.song);
    CHECK (phazite::getSongOwn (w.song).outline == "mine");
    CHECK (phazite::getLastKey (w.song) == phazite::Key { .root = 4 });
    phazite::redo (w.song);
    phazite::test::checkSong (w.song);
    CHECK (phazite::getSongOwn (w.song).outline == "theirs");
    CHECK (phazite::getLastKey (w.song) == phazite::Key { .root = 4 });

    // A version applied that changed the key: its Undo puts the key back, one set since lost, as the tree's Undo of it was.
    auto keyed = phazite::test::copyWholeSong (w.song);
    juce::ValueTree (keyed.getChildWithName (schema::LASTKEY)).setProperty (schema::keyRoot, 6, nullptr);

    {
        const phazite::UndoStep step (w.song, "Apply");
        phazite::makeSongLike (w.song, keyed);
    }

    CHECK (phazite::getLastKey (w.song)->root == 6);
    phazite::test::checkSong (w.song);
    phazite::setLastKey (w.song, { .root = 11 });
    phazite::undo (w.song);
    phazite::test::checkSong (w.song);
    CHECK (phazite::getLastKey (w.song)->root == 4);
}

//==============================================================================
TEST_CASE ("Song values' own: what plays makes a plan for the loop and the metronome alone")
{
    Watched w;
    phazite::test::PlanCounter plans (w.s.context);
    plans.take();

    struct Edit
    {
        const char* what = "";
        std::function<void()> make {};
        int plans = 0;
    };

    // In the order they are made, each on the song as the ones before left it. Before this piece the tree's callbacks made a
    // plan of each of the song's own but the play start, the grids and the versions' names: a notes edit, an outline, patterns'
    // speed and a key set last made one each (named: roadmap step S5.20, the song's own).
    const std::vector<Edit> edits {
        { "the project's notes", [&] { phazite::setProjectNotes (w.s.context, "Notes"); }, 0 },
        { "its Undo", [&] { phazite::undo (w.song); }, 0 },
        { "an outline applied", w.inStep ("Outline", [&] { phazite::test::setStepProbe (w.song, "outline"); }), 0 },
        { "patterns keep their speed", w.inStep ("Patterns", [&] { phazite::togglePatternsFollowTempo (w.song); }), 0 },
        { "a key set last, its first", [&] { phazite::setLastKey (w.song, { .root = 3 }); }, 0 },
        { "a key set last again", [&] { phazite::setLastKey (w.song, { .root = 5 }); }, 0 },
        { "the playlist's grid",
          [&] { phazite::saveGridSettings (w.song, phazite::GridOwner::playlist, withSnap (w.song, phazite::GridOwner::playlist)); }, 0 },
        { "the play start moved", [&] { phazite::movePlayhead (w.song, at (4.0)); }, 0 },
        { "punch on", [&] { phazite::setPunchOn (w.song, true); }, 0 },
        { "the loop on", [&] { phazite::toggleLoop (w.song); }, 1 },
        { "a range looped", [&] { phazite::loopBeats (w.song, { at (4.0), at (8.0) }); }, 1 },
        { "the metronome on", [&] { phazite::setMetronomeOn (w.song, true); }, 1 },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        edit.make();
        CHECK (plans.take() == edit.plans);
        phazite::test::checkSong (w.song);
    }
}

TEST_CASE ("Song values' own: what is counted makes the project unsaved and its Undo saved again; how it plays and shows never")
{
    Watched w;
    auto& project = w.s.getProject();
    REQUIRE (project.save().wasOk());
    REQUIRE_FALSE (project.hasUnsavedChanges());

    const auto check = [&project] (bool unsaved)
    {
        CHECK (project.hasUnsavedChanges() == unsaved);
        CHECK (project.isWholeAsSaved() == ! unsaved);
    };

    // In the history: each with its Undo as saved (compared, not marked), and done again unsaved.
    for (const auto& [what, change] :
         std::vector<std::pair<const char*, std::function<void()>>> {
             { "an outline", w.inStep ("Outline", [&] { phazite::test::setStepProbe (w.song, "an outline"); }) },
             { "the notes", [&] { phazite::setProjectNotes (w.s.context, "Some notes"); } },
             { "patterns' speed", w.inStep ("Patterns", [&] { phazite::togglePatternsFollowTempo (w.song); }) } })
    {
        INFO (std::string (what));
        change();
        phazite::test::checkSong (w.song);
        phazite::undo (w.song);
        phazite::test::checkSong (w.song);
        check (false);
        phazite::redo (w.song);
        phazite::test::checkSong (w.song);
        check (true);
        REQUIRE (project.save().wasOk());
        check (false);
    }

    // Outside it: the key set last, and a merge waiting, each set and set back as saved, and set unsaved.
    phazite::setLastKey (w.song, { .root = 2 });
    check (true);
    REQUIRE (project.save().wasOk());
    phazite::setLastKey (w.song, { .root = 8 });
    phazite::setLastKey (w.song, { .root = 2 });
    check (false);
    phazite::setLastKey (w.song, { .root = 8 });
    check (true);
    REQUIRE (project.save().wasOk());

    phazite::MergingRecord merge;
    merge.version = repeat ('a', 64);
    merge.head = repeat ('b', 64);
    merge.before = repeat ('c', 64);
    // Set by hand in a song with nothing to undo, as one just opened (setWaitingMerge()).
    w.song.getUndoManager().clearUndoHistory();
    phazite::setWaitingMerge (w.song, phazite::writeMergingElement (merge));
    phazite::setWaitingMerge (w.song, {});
    check (false);
    phazite::setWaitingMerge (w.song, phazite::writeMergingElement (merge));
    check (true);
    phazite::setWaitingMerge (w.song, {});
    phazite::test::checkSong (w.song);
    REQUIRE (project.save().wasOk());
    check (false);

    // How the song plays and shows: never.
    phazite::toggleLoop (w.song);
    phazite::loopBeats (w.song, { at (2.0), at (6.0) });
    phazite::setMetronomeOn (w.song, true);
    phazite::setPlayStart (w.song, at (3.0));
    phazite::setPunchOn (w.song, true);
    phazite::saveGridSettings (w.song, phazite::GridOwner::playlist, withSnap (w.song, phazite::GridOwner::playlist));
    phazite::saveGridSettings (w.song, phazite::GridOwner::pianoRoll, withSnap (w.song, phazite::GridOwner::pianoRoll));
    phazite::test::checkSong (w.song);
    check (false);
}

//==============================================================================
namespace
{
// A clash of the song's own as a merge reports one: a property of the song's (no element on its path) with its three values,
// or an element whole with its three.
phazite::MergeClash makePropertyClash (const juce::Identifier& property, juce::var base, juce::var ours, juce::var theirs)
{
    phazite::MergeClash clash;
    clash.line = "Song";
    clash.property = property;
    clash.base = std::move (base);
    clash.ours = std::move (ours);
    clash.theirs = std::move (theirs);
    return clash;
}

phazite::MergeClash makeKeyClash (int base, std::optional<int> theirs)
{
    const auto key = [] (int root)
    {
        juce::ValueTree element (schema::LASTKEY);
        element.setProperty (schema::keyRoot, root, nullptr);
        element.setProperty (schema::keyScale, "minor", nullptr);
        return element;
    };

    phazite::MergeClash clash;
    clash.line = "Song";
    clash.path.add (schema::LASTKEY.toString() + ":");
    clash.baseElement = key (base);
    clash.oursElement = key (base);
    clash.theirsElement = theirs.has_value() ? key (*theirs) : juce::ValueTree();
    return clash;
}

phazite::AreaChoice choose (phazite::Song& song, const phazite::MergeClash& clash, int value)
{
    const phazite::UndoStep step (song, "Choose");
    return phazite::chooseSongOwnClash (song, clash, value, {});
}
} // namespace

TEST_CASE ("Song values' own: a merge's choice of the song's own property or its key set last goes through the setters, in the "
           "choice's step")
{
    Watched w;
    phazite::test::setStepProbe (w.song, "ours");

    // A property: the side chosen's value, or none where it had none.
    auto clash = makePropertyClash (schema::outline, {}, "ours", "theirs");
    auto heard = w.hear ([&] { CHECK (choose (w.song, clash, 2) == phazite::AreaChoice::chosen); });
    CHECK (heard.steps == 1);
    checkOwnOnce (heard, SongOwnChange::outline);
    CHECK (phazite::getSongOwn (w.song).outline == "theirs");
    CHECK (choose (w.song, clash, 0) == phazite::AreaChoice::chosen);
    CHECK (phazite::getSongOwn (w.song).outline.isEmpty());

    CHECK (choose (w.song, makePropertyClash (schema::notes, "a", "b", "Their notes"), 2) == phazite::AreaChoice::chosen);
    CHECK (phazite::getSongOwn (w.song).notes == "Their notes");
    CHECK (choose (w.song, makePropertyClash (schema::patternsKeepTheirSpeed, {}, {}, true), 2) == phazite::AreaChoice::chosen);
    CHECK (phazite::getSongOwn (w.song).patternsKeepTheirSpeed);
    phazite::test::checkSong (w.song);

    // The key set last, whole: the other's, then none, each told as the standing, in the step.
    heard = w.hear ([&] { CHECK (choose (w.song, makeKeyClash (2, 9), 2) == phazite::AreaChoice::chosen); });
    CHECK (heard.steps == 1);
    CHECK (heard.told.standing == 1);
    REQUIRE (phazite::getLastKey (w.song).has_value());
    CHECK (*phazite::getLastKey (w.song) == phazite::Key { .root = 9, .scale = phazite::Scale::minor });
    phazite::undo (w.song);
    phazite::test::checkSong (w.song);
    CHECK_FALSE (phazite::getLastKey (w.song).has_value());
    phazite::redo (w.song);
    phazite::test::checkSong (w.song);
    CHECK (choose (w.song, makeKeyClash (2, std::nullopt), 2) == phazite::AreaChoice::chosen);
    CHECK_FALSE (phazite::getLastKey (w.song).has_value());
    phazite::test::checkSong (w.song);
}

TEST_CASE ("Song values' own: a merge's choice the song can't hold is refused, writing nothing; what isn't the song's own isn't its")
{
    Watched w;
    phazite::test::setStepProbe (w.song, "ours");
    phazite::setLastKey (w.song, { .root = 1 });
    const auto own = phazite::getSongOwn (w.song);
    const auto standing = phazite::getSongStanding (w.song);
    const auto steps = w.countSteps();

    // A numbering not of its digits, an uncounted property of the song's own, a property of the key set last, what is deeper in
    // it, a key without a root, and a path into the piano roll's grid or a merge waiting.
    auto keyProperty = makeKeyClash (1, 4);
    keyProperty.property = schema::keyRoot;
    auto deeper = makeKeyClash (1, 4);
    deeper.path.add ("NOTE:1");
    auto noRoot = makeKeyClash (1, 4);
    noRoot.theirsElement.removeProperty (schema::keyRoot, nullptr);
    auto pianoRoll = makePropertyClash (schema::gridSnap, true, true, false);
    pianoRoll.path.add (schema::PIANOROLL.toString() + ":");
    auto merging = makePropertyClash (schema::mergingChosen, "1", "1", "2");
    merging.path.add (schema::MERGING.toString() + ":");

    for (const auto& [what, clash] : std::vector<std::pair<const char*, phazite::MergeClash>> {
             { "a numbering", makePropertyClash (schema::idNumbering, {}, {}, "not hex") },
             { "the loop", makePropertyClash (schema::loop, {}, {}, true) },
             { "a key's root", keyProperty },
             { "in a key", deeper },
             { "a key without its root", noRoot },
             { "the piano roll's grid", pianoRoll },
             { "a merge waiting", merging } })
    {
        INFO (std::string (what));
        CHECK (choose (w.song, clash, 2) == phazite::AreaChoice::refused);
    }

    CHECK (phazite::getSongOwn (w.song) == own);
    CHECK (phazite::getSongStanding (w.song) == standing);
    CHECK (w.countSteps() == steps);
    phazite::test::checkSong (w.song);

    // A property Phazite doesn't know at the song's top, and another area's path: not the song's own's.
    CHECK (choose (w.song, makePropertyClash ("later", {}, {}, 1), 2) == phazite::AreaChoice::notTheArea);
    auto tempo = makePropertyClash (schema::bpm, 120.0, 120.0, 128.0);
    tempo.path.add (schema::TEMPOMAP.toString() + ":");
    CHECK (choose (w.song, tempo, 2) == phazite::AreaChoice::notTheArea);
}

//==============================================================================
TEST_CASE ("Song values' own: a gesture rolled back tells the song's own once, and leaves what was set outside the history meanwhile")
{
    Watched w;
    const auto steps = w.countSteps();
    phazite::Gesture gesture (w.song, "Drag");

    // In the gesture's step, the notes; outside the history meanwhile, the loop: each told once as it is set.
    checkOwnOnce (w.hear ([&] { phazite::setProjectNotes (w.s.context, "In a drag"); }), SongOwnChange::notes);
    checkOutsideOnce (w.hear ([&] { phazite::toggleLoop (w.song); }), true);

    // Rolled back (a move of the history, told once it is whole): the notes alone, once; the loop as it was set.
    const auto rolledBack = w.hear ([&] { gesture.rollBack(); });
    checkOwnOnce (rolledBack, SongOwnChange::notes);
    CHECK (w.countSteps() == steps); // nothing left of the gesture's step
    CHECK (phazite::getSongOwn (w.song).notes.isEmpty());
    CHECK (phazite::isLoopOn (w.song));
}

TEST_CASE ("Song values' own: a trial tells each outline tried, put back and kept once, and its Undo and Redo once each")
{
    Watched w;
    const auto steps = w.countSteps();

    {
        phazite::Trial trial (w.song, "Outline", nullptr);
        checkOwnOnce (w.hear ([&] { trial.apply ([&] { phazite::test::setStepProbe (w.song, "verse-chorus"); }); }),
                      SongOwnChange::outline);

        // Tried again from the start: what was tried taken back (a move of the history, told once it is whole), then the
        // next, told once as it is set: two tells, each of a change.
        const auto again = w.hear ([&] { trial.apply ([&] { phazite::test::setStepProbe (w.song, "verse"); }, true); });
        CHECK (again.told.own == 2);
        CHECK (again.told.parts == SongOwnChange::outline);
        CHECK (again.told.standing == 0);
        CHECK (again.told.outside == 0);
        CHECK (phazite::getSongOwn (w.song).outline == "verse");

        // Kept: nothing more to tell, one step.
        CHECK (w.hear ([&] { trial.keep ("Outline"); }).told.own == 0);
    }

    CHECK (w.countSteps() == steps + 1);
    checkOwnOnce (w.undo(), SongOwnChange::outline);
    checkOwnOnce (w.redo(), SongOwnChange::outline);

    // One put back: told once, no step, the outline kept before it.
    {
        phazite::Trial trial (w.song, "Outline", nullptr);
        checkOwnOnce (w.hear ([&] { trial.apply ([&] { phazite::test::setStepProbe (w.song, "chorus"); }); }), SongOwnChange::outline);
        checkOwnOnce (w.hear ([&] { trial.putBack(); }), SongOwnChange::outline);
    }

    CHECK (w.countSteps() == steps + 1);
    CHECK (phazite::getSongOwn (w.song).outline == "verse");
}

namespace
{
juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    REQUIRE (phazite::test::waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (phazite::test::waitForVersionHistory (context));
    return phazite::getLastVersion (context)->id;
}

void checkAtMostOnceEach (const Watched::Heard& heard)
{
    CHECK (heard.told.own <= 1);
    CHECK (heard.told.standing <= 1);
    CHECK (heard.told.outside <= 1);
}
} // namespace

TEST_CASE ("Song values' own: a version shown in the song's place and Back to Now tell each value at most once a pass")
{
    Watched w;

    // The version: its notes, its key set last and its loop; Now: others of each.
    phazite::setProjectNotes (w.s.context, "The version's");
    phazite::setLastKey (w.song, { .root = 2 });
    phazite::toggleLoop (w.song);
    const auto old = keepVersion (w.s.context, "Old");
    phazite::setProjectNotes (w.s.context, "Now's");
    phazite::setLastKey (w.song, { .root = 9 });
    phazite::toggleLoop (w.song);
    REQUIRE (phazite::test::waitForVersionHistory (w.s.context));

    // Shown (the version made the song's in the trial's step, makeSongLike(): what is counted): its notes and its key, told
    // once each; the loop, which isn't counted, Now's still.
    const auto shown = w.hear (
        [&] { REQUIRE (phazite::test::finished (w.s.context, [&] { return phazite::openVersion (w.s.context, old, w.s.save); })); });
    REQUIRE (phazite::getOpenVersion (w.s.context) == old);
    checkAtMostOnceEach (shown);
    CHECK (shown.told.own == 1);
    CHECK (shown.told.parts == SongOwnChange::notes);
    CHECK (shown.told.standing == 1);
    CHECK (shown.told.lastKey);
    CHECK (shown.told.outside == 0);
    CHECK (phazite::getSongOwn (w.song).notes == "The version's");
    CHECK (phazite::getLastKey (w.song) == phazite::Key { .root = 2 });

    // Meanwhile, outside the history: the loop on, another key.
    phazite::toggleLoop (w.song);
    phazite::setLastKey (w.song, { .root = 5 });

    // Back to Now: the trial's step undone (a move of the history: the notes and the key back, told once each), then what
    // changed without a step made as Now had it (makeSongAsCopied(): the loop; the key is Now's again already): once each.
    const auto back = w.hear ([&] { REQUIRE (phazite::backToNow (w.s.context)); });
    checkAtMostOnceEach (back);
    CHECK (back.told.own == 1);
    CHECK (back.told.parts == SongOwnChange::notes);
    CHECK (back.told.standing == 1);
    CHECK (back.told.outside == 1);
    CHECK (back.told.plays);
    CHECK (phazite::getSongOwn (w.song).notes == "Now's");
    CHECK (phazite::getLastKey (w.song) == phazite::Key { .root = 9 });
    CHECK_FALSE (phazite::isLoopOn (w.song));
}

TEST_CASE ("Song values' own: a song made the same as another, and as a copy taken before, tells each value once at most")
{
    Watched w;
    phazite::setProjectNotes (w.s.context, "Mine");
    phazite::setLastKey (w.song, { .root = 1 });

    // Another song (a branch switched to: makeSongTheSame()) that differs in the notes, the key set last and the loop: each
    // value told once.
    auto other = phazite::test::copyWholeSong (w.song);
    other.setProperty (schema::notes, "Theirs", nullptr);
    other.setProperty (schema::loop, true, nullptr);
    juce::ValueTree (other.getChildWithName (schema::LASTKEY)).setProperty (schema::keyRoot, 7, nullptr);

    const auto same = w.hear ([&] { phazite::makeSongTheSame (w.song, other); });
    CHECK (same.told.own == 1);
    CHECK (same.told.parts == SongOwnChange::notes);
    CHECK (same.told.standing == 1);
    CHECK (same.told.lastKey);
    CHECK (same.told.outside == 1);
    CHECK (same.told.plays);
    CHECK (phazite::getSongOwn (w.song).notes == "Theirs");
    CHECK (phazite::getLastKey (w.song)->root == 7);
    CHECK (phazite::isLoopOn (w.song));

    // A copy taken, then the key and the loop changed outside the history (Back to Now's makeSongAsCopied()): the standing and
    // the outside value told once each; the song's own value, which only a step writes, never.
    const auto copy = phazite::test::copyWholeSong (w.song);
    const auto copyState = phazite::getSongState (w.song);
    phazite::setLastKey (w.song, { .root = 3 });
    phazite::toggleLoop (w.song);
    const auto asCopied = w.hear ([&] { phazite::makeSongAsCopied (w.song, copyState); });
    CHECK (asCopied.told.own == 0);
    CHECK (asCopied.told.standing == 1);
    CHECK (asCopied.told.lastKey);
    CHECK (asCopied.told.outside == 1);
    CHECK (phazite::getLastKey (w.song)->root == 7);
    CHECK (phazite::isLoopOn (w.song));

    // A second pass of each: nothing told.
    const auto again = w.hear ([&] { phazite::makeSongAsCopied (w.song, copyState); });
    CHECK (again.told.own + again.told.standing + again.told.outside == 0);
}
