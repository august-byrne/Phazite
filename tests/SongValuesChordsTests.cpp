// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's chord lane as an area of its values (model/base/internal/SongValues.h, ChordLaneArea.h; roadmap step S5.20, the
// chord lane's switch). What hears the lane change hears it from one place (Listener::chordLaneChanged()), which the lane's
// area tells: for each edit made through the model, its Undo and its Redo, once for each change of the value, and nothing of
// the clips or the tempo map; another area's change tells it nothing. What listens to the song's root hears what is in the
// lane's CHORDLANE from the area alone, and the CHORDLANE's own coming and going (whether the song has a lane) from the tree.
// The keeper takes in a CHORDLANE that comes with chords and keys, and refuses a lone CHORD added in an undo step; a set equal
// to the lane held makes no step; every edit is one exact undo step; a lane made by the first chord goes with its Undo; and
// the versions give the song another lane, or none, through the lane's code, and a merge's clash on the lane is chosen
// either way.
//   phazite_tests.exe --test-case="Song values' chord lane*"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/chords/Progressions.h"
#include "model/chords/internal/Chords.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/VersionHistory.h"

#include <array>
#include <functional>
#include <initializer_list>
#include <vector>

namespace
{
using phazite::test::at;
using phazite::test::lasting;
namespace schema = phazite::schema;

// How many calls the values' listener heard, of each area.
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<phazite::TrackID>&) override { ++clips; }
    void tempoMapChanged() override { ++tempo; }
    void chordLaneChanged() override { ++chords; }

    int notes = 0, clips = 0, tempo = 0, chords = 0;
};

// What a listener of the song's root is told: inside the chord lane's element, of the element itself (added or taken out),
// and all else.
struct TreeHeard final : juce::ValueTree::Listener
{
    explicit TreeHeard (const juce::ValueTree& rootToHear)
        : root (rootToHear)
    {
        root.addListener (this);
    }

    ~TreeHeard() override { root.removeListener (this); }

    static bool isInChordLane (juce::ValueTree element)
    {
        for (; element.isValid(); element = element.getParent())
            if (element.hasType (schema::CHORDLANE))
                return true;

        return false;
    }

    void hear (const juce::ValueTree& element)
    {
        if (isInChordLane (element))
            ++inLane;
        else
            ++other;
    }

    void hearChild (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        if (child.hasType (schema::CHORDLANE))
            ++lane;
        else
            hear (parent);
    }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { hear (tree); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { hearChild (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { hearChild (parent, child); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { hear (parent); }

    juce::ValueTree root;
    int inLane = 0, lane = 0, other = 0;
};

// A song in a project of its own, with its versions, the values' listener and a listener of its root recording what they
// are told.
struct Watched
{
    Watched()
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        song.getValues()->addListener (&told);
    }

    ~Watched() { song.getValues()->removeListener (&told); }

    // What a change told: the values' listener, and the root's listener (inside the lane's element, of the element itself,
    // and of everything else); and how far the project's count of changes and the versions' count of the song's changes
    // went up meanwhile.
    struct Heard
    {
        Told told;
        int inLane = 0, lane = 0, other = 0;
        juce::int64 project = 0, versions = 0;
    };

    Heard hear (const std::function<void()>& change)
    {
        auto* versions = phazite::getVersionHistory (s.context);
        REQUIRE (versions != nullptr);
        told = {};
        tree.inLane = tree.lane = tree.other = 0;
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        change();
        return { told,
                 tree.inLane,
                 tree.lane,
                 tree.other,
                 s.getProject().getChangeCount() - projectBefore,
                 versions->countSongChanges() - versionsBefore };
    }

    // A change as one undo step, then its Undo, then its Redo: what each told.
    std::vector<Heard> hearEach (const juce::String& name, const std::function<void()>& change)
    {
        return { hear (
                     [&]
                     {
                         const phazite::UndoStep step (song, name);
                         change();
                     }),
                 hear ([this] { phazite::undo (song); }), hear ([this] { phazite::redo (song); }) };
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    Told told;
    TreeHeard tree { song.getState() };
};

const phazite::Progression& getCanon()
{
    const auto* progression = phazite::findProgression ("canon");
    REQUIRE (progression != nullptr);
    return *progression;
}

phazite::ChordLaneValue getLane (const phazite::Song& song)
{
    return phazite::getChordLaneValue (song);
}

// A CHORDLANE element with these chords (start, length, degree) and key changes (start, root), as a file holds one.
juce::ValueTree makeLaneElement (std::initializer_list<std::array<double, 3>> chords, std::initializer_list<std::array<double, 2>> keys)
{
    juce::ValueTree lane (schema::CHORDLANE);

    for (const auto& chord : chords)
        lane.appendChild (juce::ValueTree (schema::CHORD, { { schema::chordStart, chord[0] },
                                                            { schema::chordLength, chord[1] },
                                                            { schema::chordDegree, (int) chord[2] },
                                                            { schema::chordType, "major" } }),
                          nullptr);

    for (const auto& key : keys)
        lane.appendChild (
            juce::ValueTree (schema::CHORDKEY,
                             { { schema::chordStart, key[0] }, { schema::keyRoot, (int) key[1] }, { schema::keyScale, "minor" } }),
            nullptr);

    return lane;
}
} // namespace

TEST_CASE ("Song values' chord lane: each edit through the model tells the lane changed once, its Undo and its Redo too, and "
           "nothing of the clips or the tempo map")
{
    Watched w;
    auto& song = w.song;
    REQUIRE_FALSE (phazite::hasChordLane (song));

    // The edit that makes the lane (its first key) is heard twice by what counts: its CHORDLANE added by the tree, its first
    // key by the area. Every other edit only by the area, the tree telling nothing inside the lane's element.
    const auto check = [&w] (const juce::String& name, const std::function<void()>& change, bool makesTheLane = false)
    {
        INFO (name.toStdString());
        const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
        const auto heard = w.hearEach (name, change);
        REQUIRE (heard.size() == moments.size());

        for (size_t i = 0; i < heard.size(); ++i)
        {
            INFO (moments[i]);
            CHECK (heard[i].told.chords == 1); // one write of the lane's value, whatever the edit
            CHECK (heard[i].told.clips == 0);
            CHECK (heard[i].told.notes == 0);
            CHECK (heard[i].told.tempo == 0);
            CHECK (heard[i].other == 0);
            CHECK (heard[i].inLane == 0);
            CHECK (heard[i].lane == (makesTheLane ? 1 : 0));
            CHECK (heard[i].project == heard[i].told.chords + heard[i].lane);
            CHECK (heard[i].versions == heard[i].told.chords + heard[i].lane);
        }
    };

    const auto firstChordStart = [&song] { return phazite::getLaneChords (song).front().range.getStart(); };

    check ("Change Key", [&] { phazite::setLaneKey (song, at (0.0), { 2, phazite::Scale::minor }); }, true);
    check ("Add Chord", [&] { phazite::putLaneChord (song, { at (0.0), at (4.0) }, { 1, 0, phazite::ChordType::minor, 0 }); });
    check ("Apply Chord Progression", [&] { phazite::putProgression (song, getCanon(), { at (8.0), at (24.0) }); });
    check ("Change Chord", [&] { phazite::setLaneChordNumeral (song, at (0.0), { 4, 0, phazite::ChordType::major, 1 }); });
    check ("Change Chord Length", [&] { phazite::setLaneChordEnd (song, at (0.0), at (6.0)); });
    check ("Add Key Change", [&] { phazite::setLaneKey (song, at (16.0), { 5, phazite::Scale::major }); });
    check ("Insert Time", [&] { phazite::insertLaneTime (song, at (2.0), lasting (4.0)); });
    check ("Remove Time", [&] { phazite::removeLaneTime (song, { at (12.0), at (14.0) }); });
    check ("Move Time", [&] { phazite::moveLaneTime (song, { at (0.0), at (4.0) }, at (20.0)); });
    check ("Paste Chords",
           [&]
           {
               const auto copy = phazite::copyLaneChords (song, { at (8.0), at (16.0) });
               REQUIRE_FALSE (copy.chords.empty());
               phazite::pasteLaneChords (song, copy, at (40.0));
           });
    check ("Clear Chords", [&] { phazite::clearLaneRange (song, { at (40.0), at (42.0) }); });
    check ("Delete Chord", [&] { phazite::removeLaneChord (song, firstChordStart()); });
    check ("Remove Key Change", [&] { phazite::removeLaneKeyChange (song, phazite::getLaneKeys (song).back().start); });
    phazite::test::checkSong (song);

    // The numeral a chord has already, and a key change at the beat where none is: no change, nothing told, no step.
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    const auto chord = phazite::getLaneChords (song).front();
    const auto same = w.hear (
        [&]
        {
            const phazite::UndoStep step (song, "Nothing");
            phazite::setLaneChordNumeral (song, chord.range.getStart(), chord.numeral);
            phazite::removeLaneKeyChange (song, at (999.0));
        });
    CHECK (same.told.chords == 0);
    CHECK (same.project == 0);
    CHECK (same.versions == 0);
    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
}

TEST_CASE ("Song values' chord lane: another area's change tells nothing of the chord lane, nor its Undo and its Redo")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto clip = w.s.addMidiClip (track, 0.0, 4.0);
    REQUIRE (clip.isValid());

    {
        const phazite::UndoStep step (song, "Chords");
        phazite::putProgression (song, getCanon(), { at (0.0), at (16.0) });
    }

    const auto check = [&w] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        for (const auto& heard : w.hearEach (name, change))
        {
            CHECK (heard.told.chords == 0);
            CHECK (heard.inLane == 0);
        }
    };

    check ("Add Clip", [&] { CHECK (phazite::insertMidiClip (song, track, { at (8.0), at (12.0) }, nullptr).isValid()); });
    check ("Add Note", [&] { CHECK (phazite::test::addMidiNote (song, clip, 60, at (1.0), lasting (1.0)).isValid()); });
    check ("Change Tempo", [&] { phazite::setTempo (song, 100.0); });
    check ("Rename Track", [&] { phazite::setTrackName (song, track, "Renamed"); });
    check ("Add Effect", [&] { w.s.addEffect (track, phazite::devices::Equalizer::typeID); });
    check ("Add Track", [&] { CHECK (phazite::insertTrack (song, track, nullptr).isValid()); });
}

TEST_CASE ("Song values' chord lane: the keeper takes in a CHORDLANE that comes into the song with chords and keys, and one added "
           "outside the history, and refuses a lone chord added in a step")
{
    // Its safety net (SongValues.h), which the versions never leave anything to (ApplySong.cpp, Chords.cpp).
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    auto& values = *song.getValues();
    song.getUndoManager().clearUndoHistory();
    REQUIRE_FALSE (phazite::hasChordLane (song));
    phazite::test::UndoCheck undo (song);
    const auto adoptedBefore = values.countChordLaneAdopted();

    // A CHORDLANE with chords and a key, in a step: its entries the song's lane, its element empty.
    undo.check ("Take In a Chord Lane",
                [&]
                {
                    const phazite::UndoStep step (song, "Take In a Chord Lane");
                    auto lane = makeLaneElement ({ { 4.0, 4.0, 5.0 }, { 0.0, 4.0, 1.0 } }, { { 0.0, 9.0 } });

                    // What Phazite doesn't know of a lane goes with its entries, as holdValues() takes it out of the song's own.
                    lane.setProperty ("unknownToPhazite", 1, nullptr);
                    lane.appendChild (juce::ValueTree ("UNKNOWNTOPHAZITE"), nullptr);
                    song.getState().appendChild (lane, &song.getUndoManager());
                });

    REQUIRE (phazite::hasChordLane (song));
    const auto chords = phazite::getLaneChords (song);
    REQUIRE (chords.size() == 2);
    CHECK (chords[0].range.getStart() == at (0.0));
    CHECK (chords[1].numeral.degree == 5);
    CHECK (phazite::getLaneKeyAt (song, at (2.0)) == phazite::Key { 9, phazite::Scale::minor });
    CHECK (song.getState().getChildWithName (schema::CHORDLANE).getNumChildren() == 0);
    CHECK (song.getState().getChildWithName (schema::CHORDLANE).getNumProperties() == 0);
    CHECK (values.countChordLaneAdopted() == adoptedBefore + 3);
    phazite::test::checkSong (song);

    // Undone and redone: the area's own action, nothing taken in again; undone, no lane.
    phazite::undo (song);
    CHECK_FALSE (phazite::hasChordLane (song));
    CHECK (getLane (song) == phazite::ChordLaneValue {});
    phazite::redo (song);
    CHECK (phazite::getLaneChords (song).size() == 2);
    CHECK (values.countChordLaneAdopted() == adoptedBefore + 3);
    phazite::test::checkSong (song);

    // A key change added to the song's CHORDLANE outside the history: after the others, taken out again.
    {
        const phazite::Song::OutsideHistory outside (song);
        song.getState()
            .getChildWithName (schema::CHORDLANE)
            .appendChild (
                juce::ValueTree (schema::CHORDKEY, { { schema::chordStart, 8.0 }, { schema::keyRoot, 4 }, { schema::keyScale, "major" } }),
                nullptr);
    }

    CHECK (getLane (song).keys.size() == 2);
    CHECK (getLane (song).keys.back() == phazite::KeyEntry { at (8.0), 4, "major" });
    CHECK (song.getState().getChildWithName (schema::CHORDLANE).getNumChildren() == 0);
    CHECK (values.countChordLaneAdopted() == adoptedBefore + 4);
    phazite::test::checkSong (song);

    // A chord added to the song's CHORDLANE by itself in a step: refused (the lane's functions change the lane), the lane as
    // it was; the step undone takes it out again.
    const auto held = getLane (song);

    {
        const phazite::test::ExpectedAssertion refused ("ChordLaneArea.cpp");
        const phazite::UndoStep step (song, "A Lone Chord");
        song.getState()
            .getChildWithName (schema::CHORDLANE)
            .appendChild (juce::ValueTree (schema::CHORD, { { schema::chordStart, 12.0 }, { schema::chordLength, 4.0 } }),
                          &song.getUndoManager());
#if JUCE_DEBUG
        CHECK (refused.getCount() == 1);
#endif
    }

    CHECK (getLane (song).isSameAs (held));
    CHECK (values.countChordLaneAdopted() == adoptedBefore + 4);
    phazite::undo (song);
    CHECK (song.getState().getChildWithName (schema::CHORDLANE).getNumChildren() == 0);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' chord lane: a lane set equal to the one the song holds makes no undo step, tells nothing and keeps the value "
           "held")
{
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;

    {
        const phazite::UndoStep step (song, "Chords");
        phazite::putProgression (song, getCanon(), { at (0.0), at (16.0) });
    }

    auto& values = *song.getValues();
    const auto held = values.chordLane.get();
    const auto steps = song.getUndoManager().getUndoDescriptions().size();
    Told told;
    values.addListener (&told);

    // The same entries in new vectors, the lane placed as it is: alike, not the same value.
    phazite::ChordLaneValue alike;
    alike.chords = immer::flex_vector<phazite::ChordEntry> (held.chords.begin(), held.chords.end());
    alike.keys = immer::flex_vector<phazite::KeyEntry> (held.keys.begin(), held.keys.end());
    alike.placed = held.placed;
    REQUIRE_FALSE (alike.isSameAs (held));

    {
        const phazite::UndoStep step (song, "Nothing");
        values.setChordLane (song, alike);
    }

    CHECK (song.getUndoManager().getUndoDescriptions().size() == steps);
    CHECK (told.chords == 0);
    CHECK (values.chordLane.get().isSameAs (held));
    values.removeListener (&told);
}

TEST_CASE ("Song values' chord lane: the lane its first chord makes goes with its Undo, the keys the key set last again, and every "
           "edit of the lane is one exact undo step")
{
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    phazite::setLastKey (song, { 7, phazite::Scale::major });
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);

    const auto edit = [&song, &undo] (const juce::String& name, const std::function<void()>& change)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        change();
                    });
    };

    // The first chord: the lane's element added in the step (the tree's action), its value with a key from the key set last
    // and the chord (the area's action); its Undo takes both back.
    edit ("Add Chord", [&] { phazite::putLaneChord (song, { at (4.0), at (8.0) }, { 2, 0, phazite::ChordType::minor, 0 }); });
    REQUIRE (phazite::hasChordLane (song));
    CHECK (getLane (song).keys.size() == 1);
    CHECK (getLane (song).keys.front() == phazite::KeyEntry { at (0.0), 7, "major" });

    phazite::undo (song);
    CHECK_FALSE (phazite::hasChordLane (song));
    CHECK (getLane (song) == phazite::ChordLaneValue {});
    CHECK (phazite::getLaneKeyAt (song, at (6.0)) == phazite::Key { 7, phazite::Scale::major });

    // A key set last since is the one the lane starts with when it is made again.
    phazite::setLastKey (song, { 3, phazite::Scale::minor });
    phazite::redo (song);
    CHECK (getLane (song).keys.front() == phazite::KeyEntry { at (0.0), 7, "major" }); // the step as it was made
    phazite::undo (song);
    edit ("Change Key", [&] { phazite::setLaneKey (song, at (8.0), { 5, phazite::Scale::major }); });
    CHECK (getLane (song).keys.front() == phazite::KeyEntry { at (0.0), 3, "minor" });

    // Every writer of the lane, each one step.
    edit ("Apply Chord Progression", [&] { phazite::putProgression (song, getCanon(), { at (0.0), at (32.0) }); });
    edit ("Add Chord", [&] { phazite::putLaneChord (song, { at (2.0), at (6.0) }, { 5, 0, phazite::ChordType::major, 0 }); });
    edit ("Change Chord", [&] { phazite::setLaneChordNumeral (song, at (2.0), { 4, 1, phazite::ChordType::minorSeventh, 2 }); });
    edit ("Change Chord Length", [&] { phazite::setLaneChordEnd (song, at (2.0), at (11.0)); });
    edit ("Delete Chord", [&] { phazite::removeLaneChord (song, at (2.0)); });
    edit ("Clear Chords", [&] { phazite::clearLaneRange (song, { at (13.0), at (15.0) }); });
    edit ("Insert Time", [&] { phazite::insertLaneTime (song, at (6.0), lasting (3.0)); });
    edit ("Remove Time", [&] { phazite::removeLaneTime (song, { at (0.0), at (5.0) }); });
    edit ("Move Time", [&] { phazite::moveLaneTime (song, { at (8.0), at (12.0) }, at (2.0)); });
    edit ("Paste Chords",
          [&]
          {
              const auto copy = phazite::copyLaneChords (song, { at (0.0), at (10.0) });
              phazite::pasteLaneChords (song, copy, at (5.0));
          });
    edit ("Remove Key Change", [&] { phazite::removeLaneKeyChange (song, phazite::getLaneKeys (song).back().start); });
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Song values' chord lane: a version's lane, and a version without one, applied both ways, each one exact undo step")
{
    // The versions give the song another song's lane through the lane pass (ApplySong.cpp: makeChordLaneLike(), through
    // Chords.h's makeChordLaneAs()), its CHORDLANE added or taken out with it, never leaving its entries to the keeper.
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (song);

    const auto withLane = phazite::makeSong (phazite::test::copyWholeSong (song));
    REQUIRE (withLane != nullptr);

    {
        const phazite::UndoStep step (*withLane, "Chords");
        phazite::putProgression (*withLane, getCanon(), { at (0.0), at (16.0) });
        phazite::setLaneKey (*withLane, at (8.0), { 4, phazite::Scale::minor });
    }

    const auto other = phazite::test::copyWholeSong (*withLane);
    const auto without = phazite::test::copyWholeSong (song);
    const auto adopted = song.getValues()->countChordLaneAdopted();

    const auto apply = [&] (const juce::String& name, const juce::ValueTree& from, const std::function<void (const juce::ValueTree&)>& make)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        make (from);
                    });
    };

    const auto songLike = [&song] (const juce::ValueTree& from) { phazite::makeSongLike (song, from); };
    const auto ownLike = [&song] (const juce::ValueTree& from) { phazite::makeSongOwnLike (song, from); };

    apply ("The Version's Song", other, songLike);
    REQUIRE (phazite::hasChordLane (song));
    CHECK (getLane (song) == getLane (*withLane));
    phazite::test::checkSong (song);

    apply ("The Song Before", without, songLike);
    CHECK_FALSE (phazite::hasChordLane (song));
    CHECK (getLane (song) == phazite::ChordLaneValue {});
    phazite::test::checkSong (song);

    apply ("The Version's Own", other, ownLike);
    CHECK (getLane (song) == getLane (*withLane));
    apply ("The Own Before", without, ownLike);
    CHECK_FALSE (phazite::hasChordLane (song));

    // A lane that stands in for what its version couldn't read is left as the song has it: here, none.
    auto standIn = other.createCopy();
    standIn.getChildWithName (schema::CHORDLANE).setProperty (schema::versions::missing, true, nullptr);
    apply ("A Stand-in", standIn, songLike);
    CHECK_FALSE (phazite::hasChordLane (song));

    CHECK (song.getValues()->countChordLaneAdopted() == adopted);
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Song values' chord lane: a merge's clash on the chord lane is chosen each way through the lane's value")
{
    // The lane merges whole: both sides changed it, so the merge reports one clash on the whole CHORDLANE, whose choice the
    // lane's chooser makes (Chords.cpp: chooseChordLaneClash()), in the choice's step.
    const auto base = phazite::createSong();

    {
        const phazite::UndoStep step (*base, "Chords");
        phazite::putLaneChord (*base, { at (0.0), at (4.0) }, { 1, 0, phazite::ChordType::major, 0 });
    }

    const auto ours = phazite::makeSong (phazite::test::copyWholeSong (*base));
    const auto theirs = phazite::makeSong (phazite::test::copyWholeSong (*base));
    REQUIRE (ours != nullptr);
    REQUIRE (theirs != nullptr);

    {
        const phazite::UndoStep step (*ours, "Change Chord");
        phazite::setLaneChordNumeral (*ours, at (0.0), { 4, 0, phazite::ChordType::major, 0 });
    }

    {
        const phazite::UndoStep step (*theirs, "Change Chord");
        phazite::setLaneChordNumeral (*theirs, at (0.0), { 5, 0, phazite::ChordType::dominantSeventh, 0 });
    }

    const phazite::MergeSides sides { phazite::test::copyWholeSong (*base), phazite::test::copyWholeSong (*ours),
                                      phazite::test::copyWholeSong (*theirs) };
    const auto merged = phazite::mergeSongs (sides.base, sides.before, sides.theirs, [&ours] { return ours->makeID(); });
    const phazite::MergeClash* clash = nullptr;

    for (const auto& each : merged.clashes)
        if (! each.path.isEmpty() && each.path[0].startsWith (schema::CHORDLANE.toString() + ":"))
            clash = &each;

    REQUIRE (clash != nullptr);
    CHECK (clash->path.size() == 1);

    {
        const phazite::UndoStep step (*ours, "Merge");
        phazite::makeSongLike (*ours, merged.song);
    }

    phazite::test::UndoCheck undo (*ours);
    const phazite::ChordLaneValue lanes[] = { getLane (*base), getLane (*ours), getLane (*theirs) };

    for (const auto value : { 2, 0, 1 })
    {
        INFO ("value " << value);
        undo.check ("Choose",
                    [&]
                    {
                        const phazite::UndoStep step (*ours, "Choose");
                        CHECK (phazite::chooseChordLaneClash (*ours, *clash, value, sides) == phazite::AreaChoice::chosen);
                    });
        CHECK (getLane (*ours) == lanes[value]);
        phazite::test::checkSong (*ours);
    }

    // A clash elsewhere is none of the lane's; one of a property of the lane, which a merge never makes, refused.
    auto elsewhere = *clash;
    elsewhere.path = { schema::TEMPOMAP.toString() + ":" + schema::TEMPOMAP.toString() };
    CHECK (phazite::chooseChordLaneClash (*ours, elsewhere, 0, sides) == phazite::AreaChoice::notTheArea);
    auto ofProperty = *clash;
    ofProperty.property = schema::chordStart;
    CHECK (phazite::chooseChordLaneClash (*ours, ofProperty, 0, sides) == phazite::AreaChoice::refused);

    // The side chosen without a lane: none.
    auto none = *clash;
    none.theirsElement = {};
    undo.check ("Choose None",
                [&]
                {
                    const phazite::UndoStep step (*ours, "Choose None");
                    CHECK (phazite::chooseChordLaneClash (*ours, none, 2, sides) == phazite::AreaChoice::chosen);
                });
    CHECK_FALSE (phazite::hasChordLane (*ours));
    phazite::test::checkSong (*ours);
}
