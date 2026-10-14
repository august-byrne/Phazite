// SPDX-License-Identifier: AGPL-3.0-or-later
// Projects saved by earlier versions of Phazite must keep opening as the same song. A small project saved by 0.1.0 pins
// what its saved grid levels and scale numbers mean, for the format migration to come; a whole one (tests/golden/legacy,
// frozen by GoldenProjectTests.cpp) must open and save as its golden file says.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "model/arrangement/Grid.h"
#include "model/arrangement/Tracks.h"
#include "model/notes/Notes.h"
#include "model/project/Project.h"

namespace
{
// A project file as Phazite 0.1.0 saved it, without what Tracktion adds when it opens one. Its numbers are Tracktion's:
// grid levels (te::TimecodeSnapType: 7 is a 1/4 beat, 13 eight bars) and scales (te::Scale::ScaleType: 1 is minor, 7
// aeolian, 10 harmonic minor). Numbers out of range are read as the nearest in range.
constexpr const char* oldProject = R"(<?xml version="1.0" encoding="UTF-8"?>
<EDIT appVersion="0.1.0" projectID="0/5eed0b1" creationTime="1791863965976" modifiedBy="">
  <PHAZITE gridSnap="0" gridAdaptive="1" gridOffset="9" gridLevel="20" gridTriplets="0">
    <PIANOROLL gridSnap="1" gridAdaptive="0" gridOffset="0" gridLevel="7" gridTriplets="1"/>
    <LASTKEY phaziteKeyRoot="9" phaziteKeyScale="10"/>
  </PHAZITE>
  <TRACK id="1003" name="Bass" phaziteKeyRoot="2" phaziteKeyScale="1"/>
  <TRACK id="1004" name="Lead" phaziteKeyRoot="4" phaziteKeyScale="7"/>
  <TRACK id="1005" name="Odd" phaziteKeyRoot="14" phaziteKeyScale="42"/>
  <TRACK id="1006" name="Plain"/>
</EDIT>
)";

std::unique_ptr<phazite::Project> openProject (const juce::File& file)
{
    juce::String error;
    auto project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    phazite::test::settle();
    return project;
}

// A grid's fixed size, as the grid menu names it.
juce::String describeFixedGrid (phazite::GridSettings grid, const te::TempoSequence& tempo)
{
    grid.adaptive = false;
    return phazite::getGridDescription (phazite::getGridSnapType (grid, tempo, {}, 24.0), tempo, grid.triplets);
}

te::AudioTrack& findTrack (const te::Edit& edit, const juce::String& name)
{
    te::AudioTrack* found = nullptr;

    for (auto* track : te::getAudioTracks (edit))
        if (track->getName() == name)
            found = track;

    REQUIRE_MESSAGE (found != nullptr, ("no track named " + name).toStdString());
    return *found;
}

juce::String describeTrackKey (const te::Track& track)
{
    const auto key = phazite::getTrackKey (track);
    return key.has_value() ? phazite::describeKey (*key) : juce::String ("no key");
}

// What the old project holds, read through the functions the app reads it with.
void checkOldProject (const phazite::Project& project)
{
    const auto& edit = project.getEdit();

    // The playlist's grid: an offset and a level beyond the largest are read as the largest.
    const auto playlistGrid = phazite::GridSettings::load (phazite::findPhaziteState (edit));
    CHECK_FALSE (playlistGrid.snap);
    CHECK (playlistGrid.adaptive);
    CHECK (playlistGrid.adaptiveOffset == 3);
    CHECK (describeFixedGrid (playlistGrid, edit.tempoSequence) == juce::String ("8 bars"));

    // The piano roll's: a 1/4 beat in triplets.
    const auto pianoRollGrid = phazite::GridSettings::load (phazite::findPhaziteState (edit).getChildWithName ("PIANOROLL"));
    CHECK (pianoRollGrid.snap);
    CHECK_FALSE (pianoRollGrid.adaptive);
    CHECK (pianoRollGrid.triplets);
    CHECK (describeFixedGrid (pianoRollGrid, edit.tempoSequence) == juce::String ("1/6 beat"));

    // The tracks' keys: a root and a scale beyond the highest are read as the highest.
    CHECK (describeTrackKey (findTrack (edit, "Bass")) == juce::String ("D minor"));
    CHECK (describeTrackKey (findTrack (edit, "Lead")) == juce::String ("E aeolian"));
    CHECK (describeTrackKey (findTrack (edit, "Odd")) == juce::String ("B harmonic minor"));
    CHECK (describeTrackKey (findTrack (edit, "Plain")) == juce::String ("no key"));
}
} // namespace

TEST_CASE ("A project saved by Phazite 0.1.0 opens with its grids and keys")
{
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Old Song").getChildFile ("Old Song.phazite");
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    REQUIRE (file.replaceWithText (oldProject));

    auto project = openProject (file);
    checkOldProject (*project);

    // A new track gets the key set last.
    auto* added = phazite::insertTrack (project->getEdit(), nullptr, nullptr);
    REQUIRE (added != nullptr);
    const auto addedID = added->itemID;
    CHECK (describeTrackKey (*added) == juce::String ("A harmonic minor"));

    // Saved by this version and opened again, it holds the same.
    REQUIRE (project->save().wasOk());
    project = nullptr;
    project = openProject (file);
    checkOldProject (*project);

    auto* reopened = te::findTrackForID (project->getEdit(), addedID);
    REQUIRE (reopened != nullptr);
    CHECK (describeTrackKey (*reopened) == juce::String ("A harmonic minor"));
}

TEST_CASE ("Golden legacy: a whole project saved by Phazite 0.1.0 opens and saves as the same song" * doctest::test_suite ("golden"))
{
    const auto legacy = phazite::test::getGoldenFolder().getChildFile ("legacy").getChildFile ("0.1.0-representative.phazite");

    if (! legacy.existsAsFile() && phazite::test::isUpdatingGoldenFiles())
    {
        MESSAGE ("tests/golden/legacy/0.1.0-representative.phazite is missing: the representative golden project freezes it, "
                 "so run the golden tests once more.");
        return;
    }

    REQUIRE_MESSAGE (legacy.existsAsFile(), "tests/golden/legacy/0.1.0-representative.phazite is missing: restore it with git.");
    CHECK (phazite::test::readXml (legacy)->getStringAttribute ("appVersion") == juce::String ("0.1.0"));

    // In a project folder of its own, with its audio.
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Old Song").getChildFile ("Old Song.phazite");
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    REQUIRE (legacy.copyFileTo (file));
    phazite::test::writeSound (file.getSiblingFile ("Audio").getChildFile ("Tone.wav"), 2.0);

    // (Not checked yet: that it opens without unsaved changes. Its MIDI clips give it some; GoldenProjectTests.cpp, above
    // saveReopenAndSaveAgain(), says why.)
    auto project = openProject (file);
    REQUIRE (project->save().wasOk());
    project = nullptr;

    phazite::test::checkProjectGolden ("legacy/0.1.0-representative.xml", *phazite::test::readNormalisedProject (file),
                                       "tests/golden/legacy/0.1.0-representative.phazite, saved by Phazite 0.1.0, opened and saved "
                                       "by this version: tests/MigrationTests.cpp");
}
