// SPDX-License-Identifier: AGPL-3.0-or-later
// The song whole as one value (model/base/internal/SongState.h): a song's state written as its
// file's tree (writeSongTree()) is the song's whole copy (WholeState.h: copyWholeSong(), which every save, version and copy
// writes the song's file form by), element for element in their order, each property as the file writes it; and read back
// (readSongTree(), what the song takes its values from as it opens), it is the state it was, area by area (identity aside), so
// that the two converters lose nothing either way. For the golden projects, the example songs, a
// random song after each of 200 edits and the benchmark's big song; the inconsistent tests' files, the fuzzer's and every
// other song the check is asked of are compared the same way by the check itself (SongCheck.cpp: checkSongState()). And a
// state is a moment: taken, then the song edited in every area, it writes the song as it was. The state read on another
// thread while the song changes is raced in tests/LockFreeTests.cpp, and what taking one costs is gated in
// tests/PerformanceGateTests.cpp.
#include "BenchmarkSong.h"
#include "GoldenFiles.h"
#include "RandomEdits.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongState.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/WholeState.h"
#include "model/chords/ChordModel.h"
#include "model/chords/internal/Chords.h"
#include "model/commands/internal/Undo.h"
#include "model/midi/MidiLinkValues.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/project/ExampleSongs.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace phazite::test;

juce::String toText (const juce::ValueTree& song)
{
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, song);
    return text.toString();
}

// The first line two texts differ at, for a failure to show.
std::string findFirstDifference (const juce::String& a, const juce::String& b)
{
    const auto linesA = juce::StringArray::fromLines (a), linesB = juce::StringArray::fromLines (b);

    for (int i = 0; i < juce::jmax (linesA.size(), linesB.size()); ++i)
        if (linesA[i] != linesB[i])
            return "line " + std::to_string (i + 1) + ": \"" + linesA[i].toStdString() + "\" against \"" + linesB[i].toStdString() + "\"";

    return {};
}

// A song's state written, against the song's whole copy taken at the same moment; and read back, against itself.
void checkConverters (const phazite::Song& song)
{
    const auto state = phazite::getSongState (song);
    const auto written = phazite::writeSongTree (state);
    const auto writtenText = toText (written);
    const auto whole = toText (phazite::copyWholeSong (song));
    INFO ("the state written, against the song's whole copy: " << findFirstDifference (whole, writtenText));
    CHECK (writtenText == whole);

    for (const auto& difference : findStateDifferences (phazite::readSongTree (written), state))
    {
        INFO ("read back, the state holds other " << difference);
        CHECK (false);
    }
}

// A golden file opened as a project from a file is.
std::unique_ptr<phazite::Project> openGolden (const TemporaryFolder& temporary, const juce::String& name)
{
    const auto file = temporary.folder.getChildFile (name.fromLastOccurrenceOf ("/", false, false).upToFirstOccurrenceOf (".", false, false)
                                                     + ".phazite");
    REQUIRE (getGoldenFolder().getChildFile (name).copyFileTo (file));
    juce::String error;
    auto project = phazite::Project::open (file, false, error);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    return project;
}
} // namespace

TEST_CASE ("Song state: each golden project's state, written, is its whole copy, and read back, the same state")
{
    TemporaryFolder temporary;

    for (const auto* name : { "projects/representative.xml", "projects/empty.xml", "commands/script.xml" })
    {
        CAPTURE (name);
        const auto project = openGolden (temporary, name);
        checkConverters (project->getSong());
    }
}

TEST_CASE ("Song state: each example song's state, written, is its whole copy, and read back, the same state")
{
    for (const auto& example : phazite::getExampleSongs())
    {
        CAPTURE (std::string (example.id));
        TemporaryFolder temporary;
        juce::String error;
        const auto project =
            phazite::Project::create (temporary.folder, example.id, error,
                                      phazite::getExampleSongContent (getEngineContext(), example.id, getFactoryPresetsFolder()));
        REQUIRE_MESSAGE (project != nullptr, error.toStdString());
        checkConverters (project->getSong());
    }
}

TEST_CASE ("Song state: a random song's state after each of 200 edits, written, is its whole copy, and read back, the same")
{
    TemporaryFolder sounds;
    auto song = phazite::createSong();
    RandomEdits edits (*song, 20261124, sounds.folder);

    for (int i = 0; i < 200; ++i)
    {
        CAPTURE (i);
        edits.edit();
        checkConverters (*song);
    }
}

TEST_CASE ("Song state: the benchmark's song of 100 tracks, written, is its whole copy, and read back, the same state")
{
    TestSong s;
    buildBenchmarkSong (s, 100);
    checkConverters (*phazite::findSong (s.context));
}

TEST_CASE ("Song state: a state taken is a moment, which writes the song as it was after the song is edited in every area")
{
    TemporaryFolder sounds;
    auto song = phazite::createSong();
    RandomEdits edits (*song, 20261125, sounds.folder);
    edits.edit (60);

    const auto state = phazite::getSongState (*song);
    const auto then = toText (phazite::copyWholeSong (*song));
    REQUIRE (toText (phazite::writeSongTree (state)) == then);

    // Random edits (every area they reach: the tracks, clips and notes, devices and their chains, curves, modulation, sends,
    // sections, the chord lane, the tempo map, mute), and by hand each area they don't: a track's view, a MIDI link, a
    // window's place, the song's own, its standing and its outside value, its count of IDs.
    edits.edit (100);
    const auto tracks = phazite::getTrackIDs (*song);
    REQUIRE_FALSE (tracks.empty());
    {
        const phazite::UndoStep step (*song, "Edit");
        phazite::setSavedTrackHeight (*song, tracks.front(), 120);
        phazite::setTrackMuted (*song, tracks.front(), ! phazite::isTrackMuted (*song, tracks.front(), false));
        const phazite::ParameterRef volume { phazite::getFader (*song, tracks.front()),
                                             phazite::getVolumeParameterID (*song, tracks.front()) };
        phazite::linkMidiController (*song, phazite::MidiController { .channel = 2, .number = 11 }, volume);
        REQUIRE (phazite::findMidiLink (*song, volume).has_value());

        auto own = phazite::getSongOwn (*song);
        own.notes = "A moment later";
        phazite::setSongOwn (*song, std::move (own));
    }

    if (const auto& devices = phazite::getDevicesValue (*song).devices; ! devices.empty())
        phazite::setWindowPlace (*song, phazite::DeviceID::fromRaw (devices.begin()->first), phazite::WindowPlace { .x = 12, .y = 34 });

    auto standing = phazite::getSongStanding (*song);
    standing.versionsBranch = "a moment later";
    phazite::storeStandingOutsideHistory (*song, std::move (standing));

    auto outside = phazite::getSongOutside (*song);
    outside.loop = ! outside.loop;
    phazite::setSongOutside (*song, std::move (outside));
    song->makeID();

    CHECK (toText (phazite::copyWholeSong (*song)) != then);
    const auto written = toText (phazite::writeSongTree (state));
    INFO ("the state written after the edits, against the song then: " << findFirstDifference (then, written));
    CHECK (written == then);
}

TEST_CASE ("Song state: the chord lane's value says whether the song has a lane, as its file's CHORDLANE does")
{
    // ChordLaneValue.h: placed, set by the writers that add or take away the song's CHORDLANE (model/chords/Chords.cpp), with
    // whole state writing an empty CHORDLANE for a lane placed without chords or keys.
    auto song = phazite::createSong();
    CHECK_FALSE (phazite::getSongState (*song).chordLane.placed);
    CHECK_FALSE (phazite::hasChordLane (*song));

    TemporaryFolder sounds;
    RandomEdits edits (*song, 20261126, sounds.folder);

    for (int i = 0; i < 300 && ! phazite::hasChordLane (*song); ++i)
        edits.edit();

    REQUIRE (phazite::hasChordLane (*song));
    CHECK (phazite::getSongState (*song).chordLane.placed);
    CHECK (phazite::test::copyWholeSong (*song).getChildWithName (phazite::schema::CHORDLANE).isValid());
    checkSong (*song);

    // A lane without chords or keys, as a version's empty lane makes it: placed, and written as an empty CHORDLANE.
    REQUIRE (phazite::makeChordLaneAs (*song, juce::ValueTree (phazite::schema::CHORDLANE)));
    const auto state = phazite::getSongState (*song);
    CHECK (state.chordLane.placed);
    CHECK (state.chordLane.chords.empty());
    CHECK (state.chordLane.keys.empty());
    CHECK (phazite::writeSongTree (state).getChildWithName (phazite::schema::CHORDLANE).isValid());
    checkSong (*song);

    // Taken away, as a version without a lane takes it.
    REQUIRE (phazite::makeChordLaneAs (*song, {}));
    CHECK_FALSE (phazite::hasChordLane (*song));
    CHECK_FALSE (phazite::writeSongTree (phazite::getSongState (*song)).getChildWithName (phazite::schema::CHORDLANE).isValid());
    checkSong (*song);
}

//==============================================================================
// The song made of a state (Song.h: Song (SongState)): what it holds, a new
// project's first save, the empty MIDILINKS and SECTIONS that are none, the root's order in a file, and a chord lane placed
// with nothing on it.
namespace
{
phazite::BeatPosition at (double beats)
{
    return phazite::BeatPosition::fromBeats (beats);
}

// The types of a file's root's children, in their order.
std::vector<juce::String> listRootTypes (const juce::XmlElement& root)
{
    std::vector<juce::String> types;

    for (const auto* child : root.getChildIterator())
        types.push_back (child->getTagName());

    return types;
}
} // namespace

TEST_CASE ("Song state: a song made of a state holds that state, area by area, and writes it as its whole copy, with no undo step")
{
    for (const auto* name : { "projects/empty.xml", "projects/representative.xml" })
    {
        INFO (name);
        const auto xml = readXml (getGoldenFolder().getChildFile (name));
        REQUIRE (xml != nullptr);
        auto file = juce::ValueTree::fromXml (*xml);
        phazite::makeConsistent (file);
        const auto state = phazite::readSongTree (file);
        const phazite::Song::Ptr song (new phazite::Song (state));

        for (const auto& difference : findStateDifferences (phazite::getSongState (*song), state))
        {
            INFO ("the song made of the state holds other " << difference);
            CHECK (false);
        }

        CHECK (toText (phazite::copyWholeSong (*song)) == toText (phazite::writeSongTree (state)));
        CHECK_FALSE (song->getUndoManager().canUndo());
    }
}

TEST_CASE ("Song state: a new project's first save holds what its content function made, which is no undo step and no unsaved "
           "change")
{
    namespace schema = phazite::schema;
    TemporaryFolder temporary;
    juce::String error;
    phazite::TrackID added;
    const auto project = phazite::Project::create (temporary.folder, "Made", error,
                                                   [&added] (phazite::Song& song)
                                                   {
                                                       added = phazite::addTrackAtEnd (song, false, nullptr);
                                                       phazite::setTrackName (song, added, "Made");
                                                       phazite::addSectionAt (song, at (0.0), nullptr);
                                                   });
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    REQUIRE (added.isValid());
    auto& song = project->getSong();
    CHECK_FALSE (song.getUndoManager().canUndo());
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK (project->isWholeAsSaved());

    const auto saved = readXml (project->getFile());
    REQUIRE (saved != nullptr);
    const auto file = juce::ValueTree::fromXml (*saved);
    CHECK (findTrackIn (file, added)[schema::trackName].toString() == "Made");
    CHECK (file.getChildWithName (schema::SECTIONS).getNumChildren() == 1);
    checkSong (song);
}

TEST_CASE ("Song state: a file with an empty MIDILINKS and an empty SECTIONS opens without them, as saved, and saves without "
           "them")
{
    // An empty list is none: an empty MIDILINKS or SECTIONS is neither kept nor written again.
    namespace schema = phazite::schema;
    TemporaryFolder temporary;
    const auto file = temporary.folder.getChildFile ("Empty.phazite");
    auto tree = phazite::writeSongTree (phazite::makeNewSong());
    tree.appendChild (juce::ValueTree (schema::MIDILINKS), nullptr);
    tree.appendChild (juce::ValueTree (schema::SECTIONS), nullptr);
    REQUIRE (file.replaceWithText (toText (tree)));

    juce::String error;
    const auto project = phazite::Project::open (file, false, error);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK (project->isWholeAsSaved());
    const auto whole = phazite::copyWholeSong (project->getSong());
    CHECK_FALSE (whole.getChildWithName (schema::MIDILINKS).isValid());
    CHECK_FALSE (whole.getChildWithName (schema::SECTIONS).isValid());

    REQUIRE (project->save().wasOk());
    const auto saved = readXml (file);
    REQUIRE (saved != nullptr);
    CHECK (saved->getChildByName ("MIDILINKS") == nullptr);
    CHECK (saved->getChildByName ("SECTIONS") == nullptr);
}

TEST_CASE ("Song state: a song whose last section is deleted is as saved, against a copy that never had sections")
{
    // An empty list is none: a song whose last section went is the same song as one that never had sections.
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    REQUIRE (s.getProject().save().wasOk());
    REQUIRE_FALSE (phazite::copyWholeSong (song).getChildWithName (phazite::schema::SECTIONS).isValid());
    phazite::SectionID made;

    {
        const phazite::UndoStep step (song, "Add Section");
        made = phazite::addSectionAt (song, at (0.0), nullptr);
    }

    REQUIRE (made.isValid());
    CHECK_FALSE (s.getProject().isWholeAsSaved()); // asked of the whole song: hasUnsavedChanges() keeps a change until a save

    {
        const phazite::UndoStep step (song, "Delete Section");
        phazite::deleteSection (song, made);
    }

    CHECK (s.getProject().isWholeAsSaved());
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    CHECK_FALSE (phazite::copyWholeSong (song).getChildWithName (phazite::schema::SECTIONS).isValid());
}

TEST_CASE ("Song state: a saved file writes the root's children in the order of their descriptions, whatever made them")
{
    // The named change: one order, the description's (Schema.h), where it was the order the song's tree came to hold them in.
    namespace schema = phazite::schema;
    TestSong s;
    auto& song = *phazite::findSong (s.context);

    // Every kind the root holds, made in another order than the file's: a chord lane, a section, a MIDI link, a curve, the
    // piano roll's grid, the key set last and a merge waiting.
    {
        const phazite::UndoStep step (song, "Every Kind");
        REQUIRE (phazite::makeChordLaneAs (song, juce::ValueTree (schema::CHORDLANE)));
        REQUIRE (phazite::addSectionAt (song, at (0.0), nullptr).isValid());
        phazite::linkMidiController (song, { 1, 20 }, s.getVolumeParameter (s.firstTrack)); // the parameter linked before, none
    }

    REQUIRE_FALSE (phazite::getMidiLinksValue (song).links.empty());

    REQUIRE (phazite::addAutomationPoint (s.context, s.getPanParameter (s.firstTrack), { at (0.0), 0.5f }) >= 0);
    auto grid = phazite::loadGridSettings (song, phazite::GridOwner::pianoRoll);
    grid.snap = ! grid.snap;
    phazite::saveGridSettings (song, phazite::GridOwner::pianoRoll, grid);
    phazite::setLastKey (song, { .root = 3, .scale = phazite::Scale::minor });
    song.getUndoManager().clearUndoHistory(); // a merge waiting is set by hand in a song with nothing to undo
    phazite::setWaitingMerge (song, phazite::writeMergingElement ({ .version = juce::String::repeatedString ("a", 64),
                                                                    .head = juce::String::repeatedString ("b", 64),
                                                                    .before = juce::String::repeatedString ("c", 64) }));
    REQUIRE (s.getProject().save().wasOk());

    std::vector<juce::String> described;

    for (const auto* element : schema::getElements())
        if (element->goesIn (schema::PHAZITE))
            described.push_back (element->toString());

    const std::vector<juce::String> order { "LASTKEY",    "TEMPOMAP",  "PIANOROLL", "TRACKS",   "MASTER",
                                            "AUTOMATION", "MIDILINKS", "MERGING",   "SECTIONS", "CHORDLANE" };
    CHECK (described == order);
    const auto saved = readXml (s.getProject().getFile());
    REQUIRE (saved != nullptr);
    CHECK (listRootTypes (*saved) == order);
}

TEST_CASE ("Song state: a chord lane placed with nothing on it is told to what follows the song, its Undo and its Redo too, and "
           "the project is unsaved by it as before")
{
    // ChordLaneValue.h: placed, which the chord lane's teller tells (SongValues.h: chordLaneChanged()), the views in the
    // chords' group (ProjectContext.h: SongChanges::chords), the project counting it and comparing it with its saved copy.
    // The playlist shows the lane's row only once it has chords (Playlist.cpp: showChordLaneWithChords()): none for an empty
    // lane, as before.
    struct View final : phazite::ProjectContext::Listener
    {
        void projectChanged() override {}
        void songChanged (const phazite::SongChanges& changes) override { chords += changes.chords ? 1 : 0; }

        int chords = 0;
    };

    TestSong s;
    auto& song = *phazite::findSong (s.context);
    REQUIRE (s.getProject().save().wasOk());
    REQUIRE_FALSE (phazite::hasChordLane (song));
    View view;
    s.context.addListener (&view);
    const auto changes = s.getProject().getChangeCount();

    {
        const phazite::UndoStep step (song, "Chord Lane");
        REQUIRE (phazite::makeChordLaneAs (song, juce::ValueTree (phazite::schema::CHORDLANE)));
    }

    dispatchPendingMessages();
    CHECK (phazite::hasChordLane (song));
    CHECK (view.chords == 1);
    CHECK (s.getProject().getChangeCount() > changes);
    CHECK (s.getProject().hasUnsavedChanges());
    CHECK_FALSE (s.getProject().isWholeAsSaved());
    CHECK_FALSE (phazite::hasLaneChords (s.context)); // no row shown

    phazite::undo (song);
    dispatchPendingMessages();
    CHECK_FALSE (phazite::hasChordLane (song));
    CHECK (view.chords == 2);
    CHECK_FALSE (s.getProject().checkUnsavedChanges()); // hasUnsavedChanges() keeps a change until a save, or this
    CHECK (s.getProject().isWholeAsSaved());

    phazite::redo (song);
    dispatchPendingMessages();
    CHECK (phazite::hasChordLane (song));
    CHECK (view.chords == 3);
    CHECK (s.getProject().checkUnsavedChanges());
    s.context.removeListener (&view);
}

TEST_CASE ("Song state: a version read in part with a lane that names no parameter gives a song that holds together")
{
    // A lane names a parameter, always (Schema.h: laneParameter): the loader drops one without, in a song read in part too
    // (Project.cpp: makeConsistent()), and the song's reader of a row holds none (TracksOutside.h: readHeldLane()).
    namespace schema = phazite::schema;
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto kept = s.firstTrack, gone = s.addTrack ("Gone");
    auto version = phazite::copyWholeSong (song);
    auto keptElement = findTrackIn (version, kept);
    REQUIRE (keptElement.isValid());
    juce::ValueTree lanes (schema::LANES);
    lanes.appendChild (juce::ValueTree (schema::LANE, { { schema::laneDevice, keptElement[schema::channelFader] },
                                                        { schema::laneParameter, juce::String() } }),
                       nullptr);
    lanes.appendChild (juce::ValueTree (schema::LANE, { { schema::laneDevice, keptElement[schema::channelFader] } }), nullptr);
    keptElement.appendChild (lanes, nullptr);

    // The other track's piece can't be read: the version is read in part, the track a stand-in.
    const auto pieces = phazite::splitSong (version);
    std::vector<phazite::MissingPiece> missing;
    const auto joined = phazite::joinSong (
        pieces.song,
        [&pieces, gone] (const juce::String& name) -> std::optional<juce::MemoryBlock>
        {
            const auto found = pieces.pieces.find (name);

            if (found == pieces.pieces.end()
                || (found->second.toString().startsWith ("<TRACK ")
                    && found->second.toString().contains ("id=\"" + juce::String ((juce::int64) gone.getRaw()) + "\"")))
                return std::nullopt;

            return found->second;
        },
        nullptr, &missing);
    REQUIRE (joined.isValid());
    REQUIRE_FALSE (missing.empty());
    CHECK_FALSE (findTrackIn (joined, kept).getChildWithName (schema::LANES).getChildWithName (schema::LANE).isValid());

    {
        const phazite::UndoStep step (song, "Bring Back");
        phazite::makeSongLike (song, joined);
    }

    CHECK (phazite::getTrackView (song, kept).lanes.empty());
    checkSong (song);
}
