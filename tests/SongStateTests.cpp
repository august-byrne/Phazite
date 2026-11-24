// SPDX-License-Identifier: AGPL-3.0-or-later
// The song whole as one value (model/base/internal/SongState.h; roadmap step S5.20, its end's first piece): a song's state
// written as its file's tree (writeSongTree()) is what a whole copy of the song gave while its tree held the places its areas
// are written into (the old path, while both are there: a copy of the song's tree with the state's values written in,
// WholeState.h's copyWhole (tree, state)), element for element in their order, each property as the file writes it; and read
// back (readSongTree()), it is the state it was, area by area (identity aside). For the golden projects, the example songs, a
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
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongState.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/WholeState.h"
#include "model/chords/internal/Chords.h"
#include "model/commands/internal/Undo.h"
#include "model/midi/MidiLinkValues.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/internal/Mixer.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/project/ExampleSongs.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <memory>
#include <string>
#include <utility>

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

// A song's state written, against the old path's copy of the song taken at the same moment; and read back, against itself.
void checkConverters (const phazite::Song& song)
{
    const auto state = phazite::getSongState (song);
    const auto written = phazite::writeSongTree (state);
    const auto writtenText = toText (written);
    const auto before = toText (phazite::copyWhole (song.getState(), state));
    INFO ("the state written, against the old path's whole copy: " << findFirstDifference (before, writtenText));
    CHECK (writtenText == before);
    CHECK (toText (phazite::copyWholeSong (song)) == writtenText);

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

TEST_CASE ("Song state: each golden project's state, written, is its old whole copy, and read back, the same state")
{
    TemporaryFolder temporary;

    for (const auto* name : { "projects/representative.xml", "projects/empty.xml", "commands/script.xml" })
    {
        CAPTURE (name);
        const auto project = openGolden (temporary, name);
        checkConverters (project->getSong());
    }
}

TEST_CASE ("Song state: each example song's state, written, is its old whole copy, and read back, the same state")
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

TEST_CASE ("Song state: a random song's state after each of 200 edits, written, is its old whole copy, and read back, the same")
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

TEST_CASE ("Song state: the benchmark's song of 100 tracks, written, is its old whole copy, and read back, the same state")
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

TEST_CASE ("Song state: the chord lane's value says whether the song has a lane, as its tree's CHORDLANE does")
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
    CHECK (song->getState().getChildWithName (phazite::schema::CHORDLANE).isValid());
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
