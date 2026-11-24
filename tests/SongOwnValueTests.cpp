// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song holds of its own as values (roadmap step S5.20, the song's own, its first source piece: the tree still holds
// them): src/model/base/internal/SongOwnValue.h's three values (SongOwnValue, the keyed SongStanding, SongOutside), their
// conversion from and to the song's root (PHAZITE's properties, LASTKEY, PIANOROLL, MERGING), and the getters every reader of
// the song's own reads through. The golden projects' roots, a song with every field set through the model and values at
// each field's bounds, written back as the file holds them; the reader as the song holds it against the loader on a damaged
// root; the standing as a keyed value in an area (design D.2: a key set outside the history survives the Undo of a step
// that changed the versions' base, which puts the base back); and each reader of the model on the getters, reading what the
// file holds.
//   phazite_tests.exe --test-case="Song's own*"
#include "GoldenFiles.h"
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestSongOwn.h"
#include "model/arrangement/GridSettings.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/ValueArea.h"
#include "model/chords/internal/Chords.h"
#include "model/notes/NoteValues.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/VersionHistory.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::ChangeDirection;
using phazite::MergingRecord;
using phazite::SavedGrid;
using phazite::SavedKey;
using phazite::SongOutside;
using phazite::SongOwnValue;
using phazite::SongStanding;

// The root's properties the conversion reads and writes (Schema.h), and its elements.
const std::vector<const schema::PropertyDescription*>& getOwnProperties()
{
    static const std::vector<const schema::PropertyDescription*> properties {
        &schema::outline,        &schema::notes,        &schema::idNumbering, &schema::patternsKeepTheirSpeed,
        &schema::versionsBranch, &schema::versionsBase, &schema::loop,        &schema::loopFollowsSong,
        &schema::loopStart,      &schema::loopEnd,      &schema::playStart,   &schema::metronome,
        &schema::punch,          &schema::lastInput,    &schema::gridSnap,    &schema::gridAdaptive,
        &schema::gridOffset,     &schema::gridStep,     &schema::gridTriplets
    };
    return properties;
}

const std::vector<const schema::Element*>& getOwnElements()
{
    static const std::vector<const schema::Element*> elements { &schema::LASTKEY, &schema::PIANOROLL, &schema::MERGING };
    return elements;
}

// What a root holds of the song's own and beside it, as text: its properties (sorted by name, as a file writes them), then
// its first LASTKEY, PIANOROLL and MERGING, in that order (a root's children may come in any order: the conversion appends).
juce::String writeOwnText (const juce::ValueTree& root)
{
    juce::ValueTree own (root.getType());
    own.copyPropertiesFrom (root, nullptr);

    for (const auto* element : getOwnElements())
        if (const auto child = root.getChildWithName (*element); child.isValid())
            own.appendChild (child.createCopy(), nullptr);

    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, own);
    return text.toString();
}

// The root without what the conversion reads (what Phazite doesn't know, and nextID, stay).
juce::ValueTree withoutOwn (const juce::ValueTree& root)
{
    auto copy = root.createCopy();

    for (const auto* property : getOwnProperties())
        copy.removeProperty (property->getName(), nullptr);

    for (const auto* element : getOwnElements())
        for (auto child = copy.getChildWithName (*element); child.isValid(); child = copy.getChildWithName (*element))
            copy.removeChild (child, nullptr);

    return copy;
}

// A root as the song holds it: both readers read it alike, and the three values written back into it, emptied of them, give
// its text again.
void checkRoundTrip (const juce::ValueTree& root)
{
    const auto own = phazite::readHeldSongOwn (root);
    const auto standing = phazite::readHeldSongStanding (root);
    const auto outside = phazite::readHeldSongOutside (root);
    CHECK (phazite::readSongOwnValue (root) == own);
    CHECK (phazite::readSongStanding (root) == standing);
    CHECK (phazite::readSongOutside (root) == outside);

    auto written = withoutOwn (root);
    phazite::writeSongOwnInto (written, own, standing, outside);
    CHECK (writeOwnText (written) == writeOwnText (root));
}

// Values read back from a root they are written into, by both readers.
void checkWrittenAndRead (const SongOwnValue& own, const SongStanding& standing, const SongOutside& outside)
{
    juce::ValueTree root (schema::PHAZITE);
    phazite::writeSongOwnInto (root, own, standing, outside);
    CHECK (phazite::readHeldSongOwn (root) == own);
    CHECK (phazite::readHeldSongStanding (root) == standing);
    CHECK (phazite::readHeldSongOutside (root) == outside);
    CHECK (phazite::readSongOwnValue (root) == own);
    CHECK (phazite::readSongStanding (root) == standing);
    CHECK (phazite::readSongOutside (root) == outside);
}

juce::String repeat (char digit, int count)
{
    return juce::String::repeatedString (juce::String::charToString (digit), count);
}

// The standing's area, as the song's history holds it: a step in it, or a write outside it.
struct StandingHistory
{
    void inStep (const juce::String& name, const std::function<SongStanding (SongStanding)>& change)
    {
        history.beginNewTransaction (name);
        area.set (change (area.get()));
    }

    // After the step before it is whole, as a write outside the history comes at a later turn of the message loop: so that the
    // step's Undo and Redo take the keyed path (withChanges()), not the open action's taking the write in.
    void outside (const std::function<SongStanding (SongStanding)>& change)
    {
        history.beginNewTransaction();
        area.setOutsideHistory (change (area.get()));
    }

    juce::UndoManager history;
    phazite::ValueArea<SongStanding> area { history, SongStanding() };
};

SongStanding withKey (SongStanding standing, int root)
{
    standing.lastKey = SavedKey { .root = root, .scale = "minor" };
    return standing;
}

SongStanding withBase (SongStanding standing, char digit)
{
    standing.versionsBase = repeat (digit, schema::versionsBaseDigits);
    return standing;
}
} // namespace

//==============================================================================
TEST_CASE ("Song's own: the golden projects' roots read the same both ways once opened, and are written back as the file holds them")
{
    for (const auto* name : { "projects/empty.xml", "projects/representative.xml" })
    {
        INFO (name);
        const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile (name));
        REQUIRE (xml != nullptr);
        const auto song = phazite::makeSong (juce::ValueTree::fromXml (*xml));
        REQUIRE (song != nullptr);
        checkRoundTrip (phazite::test::copyWholeSong (*song));
        checkRoundTrip (song->getState());
    }

    // What the representative project holds of its own, through the getters (its versions' names are placeholders, which
    // no version can be: none as the song holds them).
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    const auto song = phazite::makeSong (juce::ValueTree::fromXml (*xml));
    REQUIRE (song != nullptr);
    const auto own = phazite::getSongOwn (*song);
    CHECK (own.outline == "golden");
    CHECK (own.notes == "Mix notes:\nturn the pad down");
    CHECK (own.patternsKeepTheirSpeed);
    CHECK_FALSE (own.merging.has_value());
    const auto standing = phazite::getSongStanding (*song);
    CHECK (standing.lastKey == std::optional (SavedKey { .root = 2, .scale = "minor" }));
    CHECK (standing.versionsBase.isEmpty());
    const auto outside = phazite::getSongOutside (*song);
    CHECK (outside.loop);
    CHECK (outside.metronome);
    CHECK (outside.loopFollowsSong == std::optional (false));
    CHECK (juce::exactlyEqual (outside.loopEnd.value_or (-1.0), 2.0));
    CHECK (outside.punch == std::optional (true));
    CHECK (outside.lastInput == "1");
    REQUIRE (outside.playlistGrid.has_value());
    CHECK (outside.playlistGrid->offset == 1);
    CHECK_FALSE (outside.playlistGrid->snap);
    REQUIRE (outside.pianoRollGrid.has_value());
    CHECK_FALSE (outside.pianoRollGrid->adaptive);
}

TEST_CASE ("Song's own: a song with every field set through the model is written back as it holds it, and its readers read it")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);

    phazite::loopBeats (song, { phazite::BeatPosition::fromBeats (4.0), phazite::BeatPosition::fromBeats (12.5) });
    phazite::setPunchOn (song, true);
    phazite::setPunchOn (song, false);
    phazite::setMetronomeOn (song, true);
    phazite::setPlayStart (song, phazite::BeatPosition::fromBeats (3.25));

    phazite::GridSettings playlist, pianoRoll;
    playlist.adaptive = false;
    playlist.fixedLevel = 12;
    pianoRoll.snap = false;
    pianoRoll.adaptiveOffset = -3;
    pianoRoll.triplets = true;
    phazite::saveGridSettings (song, phazite::GridOwner::playlist, playlist);
    phazite::saveGridSettings (song, phazite::GridOwner::pianoRoll, pianoRoll);

    phazite::setLastKey (song, phazite::Key { .root = 9, .scale = phazite::Scale::dorian });
    phazite::test::changeSongInStep (song, "an outline's id");
    phazite::setProjectNotes (s.context, "First line\nsecond line\n\nfourth");
    phazite::togglePatternsFollowTempo (song);

    MergingRecord merge;
    merge.version = repeat ('a', 64);
    merge.head = repeat ('b', 64);
    merge.before = repeat ('c', 64);
    merge.chosen = "12";
    phazite::setWaitingMerge (song, phazite::writeMergingElement (merge));

    phazite::test::checkSong (song);
    checkRoundTrip (phazite::test::copyWholeSong (song));
    checkRoundTrip (song.getState());

    // Each reader of the song's own, on the getters: what the model set.
    CHECK (phazite::isLoopOn (song));
    CHECK_FALSE (phazite::doesLoopFollowSong (song));
    CHECK (phazite::getLoopBeats (song).getStart().inBeats() == doctest::Approx (4.0));
    CHECK (phazite::getLoopBeats (song).getEnd().inBeats() == doctest::Approx (12.5));
    CHECK (phazite::getPlayStartBeat (song).inBeats() == doctest::Approx (3.25));
    CHECK (phazite::isMetronomeOn (song));
    CHECK_FALSE (phazite::isPunchOn (song));
    CHECK (phazite::getSongOutside (song).punch == std::optional (false)); // written false, as setPunchOn() writes it
    CHECK (phazite::loadGridSettings (song, phazite::GridOwner::playlist) == playlist);
    CHECK (phazite::loadGridSettings (song, phazite::GridOwner::pianoRoll) == pianoRoll);
    CHECK (phazite::getLastKey (song) == std::optional (phazite::Key { .root = 9, .scale = phazite::Scale::dorian }));
    CHECK (phazite::getLaneKeyAt (song, phazite::BeatPosition()) == phazite::Key { .root = 9, .scale = phazite::Scale::dorian });
    CHECK (phazite::getAppliedOutline (song) == "an outline's id");
    CHECK (phazite::getProjectNotes (s.context) == "First line\nsecond line\n\nfourth");
    CHECK_FALSE (phazite::doPatternsFollowTempo (song));
    CHECK (phazite::getSongOwn (song).merging == std::optional (merge));
    CHECK (phazite::readMergeState (*phazite::getSongOwn (song).merging).has_value());
    CHECK (phazite::writeMergeState (*phazite::readMergeState (merge)).isEquivalentTo (phazite::writeMergingElement (merge)));

    // And back: the loop following the song again, the metronome off, play from the start, the patterns on their beats
    // again, the notes emptied, and a merge waiting with none of its conflicts chosen yet.
    phazite::loopSong (song);
    phazite::setMetronomeOn (song, false);
    phazite::setPlayStart (song, phazite::BeatPosition());
    phazite::togglePatternsFollowTempo (song);
    phazite::setProjectNotes (s.context, {});
    MergingRecord unchosen;
    unchosen.version = merge.version;
    unchosen.head = merge.head;
    unchosen.before = merge.before;
    phazite::setWaitingMerge (song, phazite::writeMergingElement (unchosen));
    CHECK (phazite::doesLoopFollowSong (song));
    CHECK (phazite::getLoopBeats (song) == phazite::getSongLoopBeats (song));
    CHECK_FALSE (phazite::isMetronomeOn (song));
    CHECK (phazite::getPlayStartBeat (song) == phazite::BeatPosition());
    CHECK (phazite::doPatternsFollowTempo (song));
    CHECK_FALSE (song.getState().hasProperty (schema::patternsKeepTheirSpeed));
    CHECK (phazite::getProjectNotes (s.context).isEmpty());
    CHECK_FALSE (song.getState().hasProperty (schema::notes));
    CHECK (phazite::getSongOwn (song).merging == std::optional (unchosen));
    CHECK_FALSE (song.getState().getChildWithName (schema::MERGING).hasProperty (schema::mergingChosen));
    phazite::test::checkSong (song);
    checkRoundTrip (phazite::test::copyWholeSong (song));
    checkRoundTrip (song.getState());
}

TEST_CASE ("Song's own: a file's input chosen last, numbering and versions' names are read by the song's readers as it holds them")
{
    phazite::test::TestSong s;
    auto file = phazite::test::copyWholeSong (*phazite::findSong (s.context));
    file.setProperty (schema::lastInput, "3+4", nullptr);
    file.setProperty (schema::idNumbering, "0123456789abcdef", nullptr);
    file.setProperty (schema::versionsBranch, repeat ('d', schema::versionsBranchDigits), nullptr);
    file.setProperty (schema::versionsBase, repeat ('e', schema::versionsBaseDigits), nullptr);
    s.openSong (file);

    const auto& song = *phazite::findSong (s.context);
    CHECK (phazite::getLastInput (s.context) == "3+4");
    CHECK (phazite::getSongOwn (song).idNumbering == "0123456789abcdef");
    CHECK (phazite::areNumberedAlike (song, file));
    CHECK_FALSE (phazite::areNumberedAlike (song, juce::ValueTree (schema::PHAZITE)));
    const auto names = phazite::VersionHistory::readNames (phazite::getSongStanding (song));
    CHECK (names.branch == repeat ('d', schema::versionsBranchDigits));
    CHECK (names.base == repeat ('e', schema::versionsBaseDigits));
    checkRoundTrip (song.getState());
}

TEST_CASE ("Song's own: values at each field's bounds and defaults are read back as they were written")
{
    checkWrittenAndRead ({}, {}, {});

    SongOwnValue own;
    own.outline = "verse-chorus";
    own.notes = "a\nb";
    own.idNumbering = "fedcba9876543210";
    own.patternsKeepTheirSpeed = true;
    MergingRecord merge;
    merge.version = "v";
    merge.head = "h";
    merge.before = "b";
    merge.renumbered = "1:2>3";
    merge.translated = "4>5";
    merge.crossed = "6>7";
    own.merging = merge;

    SongStanding standing;
    standing.versionsBranch = repeat ('0', schema::versionsBranchDigits);
    standing.versionsBase = repeat ('f', schema::versionsBaseDigits);

    SongOutside outside;
    outside.loop = true;
    outside.metronome = true;
    outside.loopFollowsSong = true;
    outside.loopStart = 0.0;
    outside.loopEnd = schema::maxBeats;
    outside.playStart = 0.5;
    outside.punch = false;
    outside.lastInput = "1";

    for (const auto root : { 0, 11 })
    {
        for (const auto* scale : schema::keyScale.getWords())
        {
            standing.lastKey = SavedKey { .root = root, .scale = scale };
            checkWrittenAndRead (own, standing, outside);
        }
    }

    // A key without a scale (a file's: read as major) stays so.
    standing.lastKey = SavedKey { .root = 4, .scale = {} };
    checkWrittenAndRead (own, standing, outside);

    for (const auto offset : { -3, 0, 3 })
    {
        for (const auto* step : schema::gridStep.getWords())
        {
            SavedGrid grid;
            grid.snap = false;
            grid.adaptive = false;
            grid.triplets = true;
            grid.offset = offset;
            grid.step = step;
            outside.playlistGrid = grid;
            outside.pianoRollGrid = SavedGrid();
            checkWrittenAndRead (own, standing, outside);
            outside.pianoRollGrid = grid;
            outside.playlistGrid.reset();
            checkWrittenAndRead (own, standing, outside);
        }
    }

    // What Phazite's writers leave out: the defaults (the loop off, play from the start, no notes), and a merge's choices
    // before any is made.
    outside = {};
    own = {};
    own.merging = MergingRecord { .version = "v", .head = "h", .before = "b" };
    checkWrittenAndRead (own, standing, outside);
    juce::ValueTree root (schema::PHAZITE);
    phazite::writeSongOwnInto (root, own, {}, outside);
    CHECK (root.getNumProperties() == 0);
    CHECK (root.getChildWithName (schema::MERGING).getNumProperties() == 3);
}

TEST_CASE ("Song's own: a damaged root read as the song holds it is what the loader makes of it")
{
    // Each field off its range, of no word, or written otherwise: what another program could write.
    auto damaged = phazite::test::copyWholeSong (*phazite::createSong());
    damaged.setProperty (schema::idNumbering, "0123456789ABCDEF", nullptr); // capitals: no numbering
    damaged.setProperty (schema::versionsBranch, "abc", nullptr);
    damaged.setProperty (schema::versionsBase, repeat ('g', schema::versionsBaseDigits), nullptr);
    damaged.setProperty (schema::loop, "yes", nullptr);
    damaged.setProperty (schema::metronome, "false", nullptr);
    damaged.setProperty (schema::patternsKeepTheirSpeed, "true", nullptr);
    damaged.setProperty (schema::loopFollowsSong, "no", nullptr);
    damaged.setProperty (schema::loopStart, "-5", nullptr);
    damaged.setProperty (schema::loopEnd, "1e9", nullptr);
    damaged.setProperty (schema::punch, "maybe", nullptr);
    damaged.setProperty (schema::gridSnap, "false", nullptr);
    damaged.setProperty (schema::gridOffset, "2.6", nullptr);
    damaged.setProperty (schema::gridStep, "1/3 beat", nullptr);
    damaged.setProperty (schema::gridAdaptive, "1", nullptr);
    damaged.setProperty (schema::gridTriplets, "0", nullptr);
    damaged.setProperty (schema::outline, "golden", nullptr);
    damaged.setProperty (schema::notes, "  spaced  ", nullptr);
    juce::ValueTree pianoRoll (schema::PIANOROLL);
    pianoRoll.setProperty (schema::gridOffset, "-9", nullptr);
    pianoRoll.setProperty (schema::gridStep, "huge", nullptr);
    pianoRoll.setProperty (schema::gridAdaptive, "0", nullptr);
    pianoRoll.setProperty (schema::gridSnap, "1", nullptr);
    pianoRoll.setProperty (schema::gridTriplets, "1", nullptr);
    damaged.appendChild (pianoRoll, nullptr);
    juce::ValueTree key (schema::LASTKEY);
    key.setProperty (schema::keyRoot, "14.4", nullptr);
    key.setProperty (schema::keyScale, "bogus", nullptr);
    damaged.appendChild (key, nullptr);

    const phazite::Song::Ptr loaded (new phazite::Song (damaged.createCopy()));
    phazite::makeConsistent (*loaded);
    const auto consistent = loaded->getState();

    CHECK (phazite::readSongOwnValue (consistent) == phazite::readHeldSongOwn (damaged));
    CHECK (phazite::readSongStanding (consistent) == phazite::readHeldSongStanding (damaged));
    CHECK (phazite::readSongOutside (consistent) == phazite::readHeldSongOutside (damaged));

    // Written back as the song holds it: what the loader left, a grid's step of no word, which it took out, completed with its
    // default, as a grid saved writes all five (Grid.h: makeSavedGrid()).
    CHECK (consistent[schema::gridStep].toString() == "1/4 beat");
    CHECK (consistent.getChildWithName (schema::PIANOROLL)[schema::gridStep].toString() == "1/4 beat");
    CHECK_FALSE (phazite::keepSongOwnAsHeld (consistent.createCopy()));
    checkRoundTrip (consistent);

    // What each became.
    const auto own = phazite::readHeldSongOwn (damaged);
    CHECK (own.idNumbering.isEmpty());
    CHECK (own.patternsKeepTheirSpeed);
    CHECK (own.notes == "  spaced  ");
    const auto standing = phazite::readHeldSongStanding (damaged);
    CHECK (standing.versionsBranch.isEmpty());
    CHECK (standing.versionsBase.isEmpty());
    CHECK (standing.lastKey == std::optional (SavedKey { .root = 11, .scale = {} }));
    const auto outside = phazite::readHeldSongOutside (damaged);
    CHECK (outside.loop);
    CHECK_FALSE (outside.metronome);
    CHECK (outside.loopFollowsSong == std::optional (false));
    CHECK (juce::exactlyEqual (outside.loopStart.value_or (-1.0), 0.0));
    CHECK (juce::exactlyEqual (outside.loopEnd.value_or (-1.0), schema::maxBeats));
    CHECK (outside.punch == std::optional (false));
    REQUIRE (outside.playlistGrid.has_value());
    CHECK_FALSE (outside.playlistGrid->snap);
    CHECK (outside.playlistGrid->offset == 3);
    CHECK (outside.playlistGrid->step == "1/4 beat");
    REQUIRE (outside.pianoRollGrid.has_value());
    CHECK (outside.pianoRollGrid->offset == -3);
    CHECK (outside.pianoRollGrid->step == "1/4 beat");
    CHECK_FALSE (outside.pianoRollGrid->adaptive);

    // A step of no word and the default read the same grid, as they did before the description had the words.
    CHECK (phazite::loadGridSettings (phazite::readSavedGrid (pianoRoll)) == phazite::loadGridSettings (*outside.pianoRollGrid));
}

TEST_CASE ("Song's own: where playback starts is held within its range, past 0 or not at all; a project opens at the start")
{
    auto file = phazite::test::copyWholeSong (*phazite::createSong());

    for (const auto& [text, held] : { std::pair ("8", 8.0), std::pair ("-2", 0.0), std::pair ("4abc", 0.0), std::pair ("0", 0.0),
                                      std::pair ("2e6", schema::maxBeats) })
    {
        INFO (text);
        file.setProperty (schema::playStart, text, nullptr);
        CHECK (juce::exactlyEqual (phazite::readHeldSongOutside (file).playStart, held));

        // The loader keeps it so (Schema.h: its range; written only past 0), and both readers then read it alike.
        phazite::Song loaded (file.createCopy());
        phazite::makeConsistent (loaded);
        const auto root = loaded.getState();
        CHECK (root.hasProperty (schema::playStart) == (held > 0.0));
        CHECK (phazite::readSongOutside (root) == phazite::readHeldSongOutside (file));
        checkRoundTrip (root);
    }

    file.setProperty (schema::playStart, 8.0, nullptr);
    const auto opened = phazite::makeSong (file);
    REQUIRE (opened != nullptr);
    CHECK (phazite::getPlayStartBeat (*opened) == phazite::BeatPosition());
}

namespace
{
// A new song's file, its root damaged as another program could write it, opened: by the loader alone (makeConsistent(),
// whose root keeps where playback starts), and as a project opens it (makeSong(), checked as every song is), its whole copy.
struct Opened
{
    juce::ValueTree byLoader {}, asProject {};
    phazite::Song::Ptr song {};
};

Opened openDamaged (const std::function<void (juce::XmlElement&)>& damage)
{
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, phazite::test::copyWholeSong (*phazite::createSong()));
    auto xml = juce::parseXML (text.toString());
    REQUIRE (xml != nullptr);
    damage (*xml);
    const auto file = juce::ValueTree::fromXml (*xml);

    Opened opened;
    phazite::Song loaded (file.createCopy());
    phazite::makeConsistent (loaded);
    opened.byLoader = loaded.getState().createCopy();
    opened.song = phazite::makeSong (file.createCopy());
    REQUIRE (opened.song != nullptr);
    phazite::test::checkSong (*opened.song);
    opened.asProject = phazite::test::copyWholeSong (*opened.song);

    // Either way, the root read as held and written back is itself.
    for (const auto& root : { opened.byLoader, opened.asProject })
    {
        CHECK_FALSE (phazite::keepSongOwnAsHeld (root.createCopy()));
        checkRoundTrip (root);
    }

    return opened;
}

int countChildren (const juce::ValueTree& root, const schema::Element& type)
{
    auto count = 0;

    for (const auto& child : root)
        count += child.hasType (type) ? 1 : 0;

    return count;
}
} // namespace

TEST_CASE ("Song's own: each form of a damaged root opens as the song holds it, read as held and written back as itself")
{
    const juce::String version = repeat ('a', 64), head = repeat ('b', 64), before = repeat ('c', 64);

    SUBCASE ("a switch written off goes: written only while on")
    {
        const auto opened = openDamaged (
            [] (juce::XmlElement& root)
            {
                root.setAttribute ("loop", "0");
                root.setAttribute ("metronome", "false");
                root.setAttribute ("patternsKeepTheirSpeed", "0");
            });

        for (const auto& root : { opened.byLoader, opened.asProject })
            for (const auto* property : { &schema::loop, &schema::metronome, &schema::patternsKeepTheirSpeed })
                CHECK_FALSE (root.hasProperty (*property));
    }

    SUBCASE ("an empty text goes")
    {
        const auto opened = openDamaged (
            [] (juce::XmlElement& root)
            {
                root.setAttribute ("notes", "");
                root.setAttribute ("outline", "");
                root.setAttribute ("lastInput", "");
            });

        for (const auto& root : { opened.byLoader, opened.asProject })
            for (const auto* property : { &schema::notes, &schema::outline, &schema::lastInput })
                CHECK_FALSE (root.hasProperty (*property));
    }

    SUBCASE ("where playback starts: none at 0 or below or of no number, else within its range")
    {
        for (const auto* text : { "-3", "0", "soon" })
        {
            INFO (text);
            CHECK_FALSE (openDamaged ([text] (juce::XmlElement& root) { root.setAttribute ("playStart", text); })
                             .byLoader.hasProperty (schema::playStart));
        }

        const auto past = openDamaged ([] (juce::XmlElement& root) { root.setAttribute ("playStart", "5e9"); });
        CHECK (juce::exactlyEqual ((double) past.byLoader[schema::playStart], schema::maxBeats));
        CHECK_FALSE (past.asProject.hasProperty (schema::playStart)); // a project opens at the start
    }

    SUBCASE ("the playlist's grid with some of its properties is completed with the others' defaults; a step of no word goes")
    {
        const auto opened = openDamaged (
            [] (juce::XmlElement& root)
            {
                root.setAttribute ("gridSnap", "0");
                root.setAttribute ("gridStep", "1/3 beat");
            });

        for (const auto& root : { opened.byLoader, opened.asProject })
        {
            CHECK_FALSE ((bool) root[schema::gridSnap]);
            CHECK ((bool) root[schema::gridAdaptive]);
            CHECK ((int) root[schema::gridOffset] == 0);
            CHECK (root[schema::gridStep].toString() == "1/4 beat");
            CHECK_FALSE ((bool) root[schema::gridTriplets]);
        }

        // A step of no word alone: no grid is left.
        const auto stepAlone = openDamaged ([] (juce::XmlElement& root) { root.setAttribute ("gridStep", "huge"); });

        using Described = const schema::PropertyDescription*;

        for (const auto* property : { Described (&schema::gridSnap), Described (&schema::gridAdaptive), Described (&schema::gridOffset),
                                      Described (&schema::gridStep), Described (&schema::gridTriplets) })
            CHECK_FALSE (stepAlone.asProject.hasProperty (property->getName()));

        CHECK_FALSE (phazite::getSongOutside (*stepAlone.song).playlistGrid.has_value());
    }

    SUBCASE ("PIANOROLL: without a grid's property it goes, with some it is completed")
    {
        const auto empty =
            openDamaged ([] (juce::XmlElement& root) { root.createNewChildElement ("PIANOROLL")->setAttribute ("gridStep", "x"); });
        CHECK_FALSE (empty.asProject.getChildWithName (schema::PIANOROLL).isValid());
        CHECK_FALSE (empty.byLoader.getChildWithName (schema::PIANOROLL).isValid());

        const auto some =
            openDamaged ([] (juce::XmlElement& root) { root.createNewChildElement ("PIANOROLL")->setAttribute ("gridOffset", "2"); });
        const auto pianoRoll = some.asProject.getChildWithName (schema::PIANOROLL);
        REQUIRE (pianoRoll.isValid());
        CHECK (pianoRoll.getNumProperties() == 5);
        CHECK ((int) pianoRoll[schema::gridOffset] == 2);
        CHECK (pianoRoll[schema::gridStep].toString() == "1/4 beat");
    }

    SUBCASE ("LASTKEY: without a root it goes; a root without a scale stays so, read as major")
    {
        for (const auto* scale : { "", "minor" })
        {
            INFO (scale);
            const auto opened = openDamaged (
                [scale] (juce::XmlElement& root)
                {
                    auto* key = root.createNewChildElement ("LASTKEY");

                    if (juce::String (scale).isNotEmpty())
                        key->setAttribute ("keyScale", scale);
                });
            CHECK_FALSE (opened.asProject.getChildWithName (schema::LASTKEY).isValid());
            CHECK_FALSE (phazite::getLastKey (*opened.song).has_value());
        }

        const auto notANumber = openDamaged (
            [] (juce::XmlElement& root)
            {
                auto* key = root.createNewChildElement ("LASTKEY");
                key->setAttribute ("keyRoot", "C");
                key->setAttribute ("keyScale", "minor");
            });
        CHECK_FALSE (notANumber.byLoader.getChildWithName (schema::LASTKEY).isValid());

        const auto rootAlone =
            openDamaged ([] (juce::XmlElement& root) { root.createNewChildElement ("LASTKEY")->setAttribute ("keyRoot", "4"); });
        const auto key = rootAlone.asProject.getChildWithName (schema::LASTKEY);
        REQUIRE (key.isValid());
        CHECK (key.getNumProperties() == 1);
        CHECK (phazite::getLastKey (*rootAlone.song) == std::optional (phazite::Key { .root = 4, .scale = phazite::Scale::major }));
    }

    SUBCASE ("the loop's range: without both ends, or its end not past its start, it goes, and the song is looped")
    {
        for (const auto& ends : { std::pair ("4", "4"), std::pair ("8", "2"), std::pair ("2", ""), std::pair ("", "6") })
        {
            INFO (ends.first << " to " << ends.second);
            const auto opened = openDamaged (
                [ends] (juce::XmlElement& root)
                {
                    root.setAttribute ("loop", "1");
                    root.setAttribute ("loopFollowsSong", "0");

                    for (const auto& [name, text] : { std::pair ("loopStart", ends.first), std::pair ("loopEnd", ends.second) })
                        if (juce::String (text).isNotEmpty())
                            root.setAttribute (name, text);
                });

            for (const auto& root : { opened.byLoader, opened.asProject })
            {
                CHECK_FALSE (root.hasProperty (schema::loopStart));
                CHECK_FALSE (root.hasProperty (schema::loopEnd));
            }

            CHECK (phazite::getLoop (*opened.song) == std::optional (phazite::getSongLoopBeats (*opened.song)));
        }

        // A range past its start stays.
        const auto kept = openDamaged (
            [] (juce::XmlElement& root)
            {
                root.setAttribute ("loopFollowsSong", "0");
                root.setAttribute ("loopStart", "2");
                root.setAttribute ("loopEnd", "6");
            });
        CHECK (phazite::getLoopBeats (*kept.song).getEnd().inBeats() == doctest::Approx (6.0));
    }

    SUBCASE ("MERGING: an empty text goes; one without its version, head or before is no merge, and stays as it is")
    {
        const auto opened = openDamaged (
            [&] (juce::XmlElement& root)
            {
                auto* merging = root.createNewChildElement ("MERGING");
                merging->setAttribute ("version", version);
                merging->setAttribute ("head", head);
                merging->setAttribute ("before", before);

                for (const auto* name : { "chosen", "renumbered", "translated", "crossed" })
                    merging->setAttribute (name, "");
            });
        const auto merging = opened.asProject.getChildWithName (schema::MERGING);
        REQUIRE (merging.isValid());
        CHECK (merging.getNumProperties() == 3);
        REQUIRE (phazite::getSongOwn (*opened.song).merging.has_value());
        CHECK (phazite::readMergeState (*phazite::getSongOwn (*opened.song).merging).has_value());

        const auto broken = openDamaged (
            [&] (juce::XmlElement& root)
            {
                auto* element = root.createNewChildElement ("MERGING");
                element->setAttribute ("version", "");
                element->setAttribute ("head", head);
            });
        const auto kept = broken.asProject.getChildWithName (schema::MERGING);
        REQUIRE (kept.isValid());
        CHECK (kept.getNumProperties() == 1);
        CHECK (kept[schema::mergingHead].toString() == head);
        REQUIRE (phazite::getSongOwn (*broken.song).merging.has_value());
        CHECK_FALSE (phazite::readMergeState (*phazite::getSongOwn (*broken.song).merging).has_value());

        const auto bare = openDamaged ([] (juce::XmlElement& root) { root.createNewChildElement ("MERGING"); });
        CHECK (bare.asProject.getChildWithName (schema::MERGING).isValid());
    }

    SUBCASE ("two of LASTKEY, PIANOROLL or MERGING: the first stays")
    {
        const auto opened = openDamaged (
            [&] (juce::XmlElement& root)
            {
                for (const auto* keyRoot : { "3", "7" })
                    root.createNewChildElement ("LASTKEY")->setAttribute ("keyRoot", keyRoot);

                for (const auto* offset : { "1", "-1" })
                    root.createNewChildElement ("PIANOROLL")->setAttribute ("gridOffset", offset);

                for (const auto& first : { version, head })
                {
                    auto* merging = root.createNewChildElement ("MERGING");
                    merging->setAttribute ("version", first);
                    merging->setAttribute ("head", head);
                    merging->setAttribute ("before", before);
                }
            });

        for (const auto& root : { opened.byLoader, opened.asProject })
        {
            CHECK (countChildren (root, schema::LASTKEY) == 1);
            CHECK (countChildren (root, schema::PIANOROLL) == 1);
            CHECK (countChildren (root, schema::MERGING) == 1);
            CHECK ((int) root.getChildWithName (schema::LASTKEY)[schema::keyRoot] == 3);
            CHECK ((int) root.getChildWithName (schema::PIANOROLL)[schema::gridOffset] == 1);
            CHECK (root.getChildWithName (schema::MERGING)[schema::mergingVersion].toString() == version);
        }
    }
}

TEST_CASE ("Song's own: the grid's saved steps are the words its description gives")
{
    const auto& words = schema::gridStep.getWords();
    REQUIRE (words.size() == (size_t) (phazite::GridSettings::maxFixedLevel - phazite::GridSettings::minFixedLevel + 1));

    for (auto level = phazite::GridSettings::minFixedLevel; level <= phazite::GridSettings::maxFixedLevel; ++level)
    {
        const auto step = phazite::getSavedGridStep (level);
        CHECK (step == words[(size_t) (level - phazite::GridSettings::minFixedLevel)]);
        CHECK (phazite::findFixedGridLevel (step) == std::optional (level));
    }

    CHECK (phazite::getDefaultGridStep() == *schema::gridStep.getDefaultValue());
}

//==============================================================================
TEST_CASE ("Song's own, keyed standing: findChanges() and withChanges() move only the entries that changed")
{
    const auto before = withBase (withKey ({}, 2), 'a');
    auto after = withKey (before, 5);
    after.versionsBranch = repeat ('1', schema::versionsBranchDigits);

    const auto changes = phazite::findChanges (before, after);
    CHECK (changes.lastKey.has_value());
    CHECK (changes.versionsBranch.has_value());
    CHECK_FALSE (changes.versionsBase.has_value());
    CHECK (phazite::withChanges (before, changes, ChangeDirection::forward) == after);
    CHECK (phazite::withChanges (after, changes, ChangeDirection::back) == before);

    // Onto a value whose base moved since: the base stays.
    CHECK (phazite::withChanges (withBase (after, 'b'), changes, ChangeDirection::back) == withBase (before, 'b'));
}

TEST_CASE ("Song's own, keyed standing (design D.2): a key set outside the history survives the Undo of Take the Newer Versions, "
           "which puts the base back")
{
    StandingHistory standing;
    standing.outside ([] (SongStanding s) { return withBase (withKey (s, 1), 'a'); });
    standing.inStep ("Take the Newer Versions", [] (SongStanding s) { return withBase (s, 'b'); });
    standing.outside ([] (SongStanding s) { return withKey (s, 7); });

    standing.history.undo();
    CHECK (standing.area.get() == withBase (withKey ({}, 7), 'a'));
    standing.history.redo();
    CHECK (standing.area.get() == withBase (withKey ({}, 7), 'b'));

    // The base the versions write outside the history after a step that set a key (a version applied): the step's Undo puts
    // the key back and leaves the base; its Redo the step's key.
    standing.inStep ("Bring Back", [] (SongStanding s) { return withKey (s, 3); });
    standing.outside ([] (SongStanding s) { return withBase (s, 'c'); });
    standing.history.undo();
    CHECK (standing.area.get() == withBase (withKey ({}, 7), 'c'));
    standing.history.redo();
    CHECK (standing.area.get() == withBase (withKey ({}, 3), 'c'));

    // A key set by hand after a step that set one is lost by its Undo, as the tree's Undo of a property loses it.
    standing.inStep ("A Merge's Choice", [] (SongStanding s) { return withKey (s, 4); });
    standing.outside ([] (SongStanding s) { return withKey (s, 10); });
    standing.history.undo();
    CHECK (standing.area.get() == withBase (withKey ({}, 3), 'c'));
}

//==============================================================================
TEST_CASE ("Song's own gate: the getters, and the readers a tick polls, allocate nothing, the texts shared with the song's tree")
{
    // The representative project: every field of the song's own held, both grids, the notes, the key, the input chosen last.
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    const auto song = phazite::makeSong (juce::ValueTree::fromXml (*xml));
    REQUIRE (song != nullptr);
    MergingRecord merge;
    merge.version = repeat ('a', 64);
    merge.head = repeat ('b', 64);
    merge.before = repeat ('c', 64);
    phazite::setWaitingMerge (*song, phazite::writeMergingElement (merge));

    auto read = 0;
    const auto allocations = phazite::test::countAllocations (
        [&song, &read]
        {
            read += phazite::getSongOwn (*song).merging.has_value() ? 1 : 0;
            read += phazite::getSongStanding (*song).lastKey.has_value() ? 1 : 0;
            read += phazite::getSongOutside (*song).pianoRollGrid.has_value() ? 1 : 0;
            read += phazite::isLoopOn (*song) ? 1 : 0;  // the top bar's tick
            read += phazite::isPunchOn (*song) ? 1 : 0; // the time line's tick
            read += phazite::canPunch (*song) ? 1 : 0;
            read += phazite::loadGridSettings (*song, phazite::GridOwner::playlist).snap ? 0 : 1;
            read += phazite::getAppliedOutline (*song).isNotEmpty() ? 1 : 0;
        });

    CHECK (read == 8);
    CHECK (allocations.value_or (0) == 0);
}
