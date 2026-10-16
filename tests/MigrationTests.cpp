// SPDX-License-Identifier: AGPL-3.0-or-later
// Projects saved by earlier versions of Phazite must keep opening as the same song. Older formats are upgraded in one place
// (src/model/base/Migrations.h): a small project saved by 0.1.0 pins what its grid levels and scale numbers meant, and a
// whole one (tests/golden/legacy, frozen by GoldenProjectTests.cpp) must open and save as its golden file says.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Grid.h"
#include "model/arrangement/Tracks.h"
#include "model/base/Migrations.h"
#include "model/base/Schema.h"
#include "model/notes/Notes.h"
#include "model/project/Project.h"

#include <algorithm>
#include <utility>

namespace
{
// A project file as Phazite 0.1.0 saved it (format version 0), without what Tracktion adds when it opens one. Its numbers
// are Tracktion's: grid levels (te::TimecodeSnapType: 7 is a 1/4 beat, 13 eight bars) and scales (te::Scale::ScaleType:
// 1 is minor, 7 aeolian, which has minor's notes, 10 harmonic minor). Numbers out of range are read as the nearest.
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
    return phazite::getGridDescription (phazite::getGridSnapType (grid, tempo, {}, 24.0, 20.0), tempo, grid.triplets);
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
    const auto playlistGrid = phazite::loadGridSettings (edit, phazite::GridOwner::playlist);
    CHECK_FALSE (playlistGrid.snap);
    CHECK (playlistGrid.adaptive);
    CHECK (playlistGrid.adaptiveOffset == 3);
    CHECK (describeFixedGrid (playlistGrid, edit.tempoSequence) == juce::String ("8 bars"));

    // The piano roll's: a 1/4 beat in triplets.
    const auto pianoRollGrid = phazite::loadGridSettings (edit, phazite::GridOwner::pianoRoll);
    CHECK (pianoRollGrid.snap);
    CHECK_FALSE (pianoRollGrid.adaptive);
    CHECK (pianoRollGrid.triplets);
    CHECK (describeFixedGrid (pianoRollGrid, edit.tempoSequence) == juce::String ("1/6 beat"));

    // The tracks' keys: a root and a scale beyond the highest are read as the highest.
    CHECK (describeTrackKey (findTrack (edit, "Bass")) == juce::String ("D minor"));
    CHECK (describeTrackKey (findTrack (edit, "Lead")) == juce::String ("E minor")); // aeolian, by its notes
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
    const auto legacyXml = phazite::test::readXml (legacy);
    CHECK (legacyXml->getStringAttribute ("appVersion") == juce::String ("0.1.0"));
    REQUIRE (legacyXml->getChildByName ("PHAZITE") != nullptr);
    CHECK_FALSE (legacyXml->getChildByName ("PHAZITE")->hasAttribute ("schemaVersion")); // format version 0

    // In a project folder of its own, with its audio.
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Old Song").getChildFile ("Old Song.phazite");
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    REQUIRE (legacy.copyFileTo (file));
    phazite::test::writeSound (file.getSiblingFile ("Audio").getChildFile ("Tone.wav"), 2.0);

    auto project = openProject (file);
    CHECK_FALSE (project->hasUnsavedChanges()); // opening isn't a change
    REQUIRE (project->save().wasOk());
    project = nullptr;

    phazite::test::checkProjectGolden ("legacy/0.1.0-representative.xml", *phazite::test::readNormalisedProject (file),
                                       "tests/golden/legacy/0.1.0-representative.phazite, saved by Phazite 0.1.0, opened and saved "
                                       "by this version: tests/MigrationTests.cpp");
}

TEST_CASE ("A project from before format versions keeps its grids and keys, now saved by name")
{
    namespace schema = phazite::schema;
    auto state = juce::ValueTree::fromXml (R"(<EDIT appVersion="0.1.0">
  <PHAZITE gridAdaptive="0" gridLevel="10" gridTriplets="1" outline="pop">
    <PIANOROLL gridLevel="5"/>
    <LASTKEY phaziteKeyRoot="9" phaziteKeyScale="1"/>
  </PHAZITE>
  <TRACK id="1" phaziteKeyRoot="2" phaziteKeyScale="3" phaziteHeight="96"/>
  <FOLDERTRACK id="2" phaziteKeyRoot="0" phaziteKeyScale="10">
    <TRACK id="3" phaziteKeyRoot="5" phaziteKeyScale="9"/>
  </FOLDERTRACK>
</EDIT>)");
    REQUIRE (state.isValid());
    CHECK (phazite::getSchemaVersion (state) == 0);

    REQUIRE (phazite::migrateToCurrentSchema (state));
    CHECK (phazite::getSchemaVersion (state) == phazite::schema::currentVersion);

    // The grids' fixed sizes by name, and nothing else of them changed.
    const auto phaziteState = state.getChildWithName (schema::PHAZITE);
    const auto pianoRoll = phaziteState.getChildWithName (schema::PIANOROLL);
    CHECK (phaziteState[schema::gridStep].toString() == juce::String ("1 bar"));
    CHECK (pianoRoll[schema::gridStep].toString() == juce::String ("1/16 beat"));
    CHECK_FALSE (phaziteState.hasProperty ("gridLevel"));
    CHECK_FALSE (pianoRoll.hasProperty ("gridLevel"));
    CHECK (phaziteState[schema::gridTriplets].toString() == juce::String ("1"));
    CHECK (phaziteState[schema::outline].toString() == juce::String ("pop"));
    CHECK (phazite::GridSettings::load (phaziteState).fixedLevel == 10);
    CHECK (phazite::GridSettings::load (pianoRoll).fixedLevel == 5);

    // Every key's scale by name: the last key set, a track's, a folder's and one inside a folder.
    const auto scaleOf = [] (const juce::ValueTree& tree) { return tree[schema::phaziteKeyScale].toString(); };
    const auto folder = state.getChild (2);
    CHECK (scaleOf (phaziteState.getChildWithName (schema::LASTKEY)) == juce::String ("minor"));
    CHECK (scaleOf (state.getChild (1)) == juce::String ("dorian"));
    CHECK (scaleOf (folder) == juce::String ("harmonicMinor"));
    CHECK (scaleOf (folder.getChild (0)) == juce::String ("melodicMinor"));
    CHECK (state.getChild (1)[schema::phaziteHeight].toString() == juce::String ("96"));
}

TEST_CASE ("Old grid levels and scale numbers upgrade to what the old version read")
{
    namespace schema = phazite::schema;

    // A version-0 state with a grid level and a key's scale, upgraded: what each became.
    const auto upgrade = [] (const juce::var& level, const juce::var& scale)
    {
        juce::ValueTree state ("EDIT"), phaziteState (schema::PHAZITE), track ("TRACK");
        phaziteState.setProperty ("gridLevel", level, nullptr);
        track.setProperty (schema::phaziteKeyRoot, 0, nullptr);
        track.setProperty (schema::phaziteKeyScale, scale, nullptr);
        state.appendChild (phaziteState, nullptr);
        state.appendChild (track, nullptr);
        REQUIRE (phazite::migrateToCurrentSchema (state));
        return std::pair { phaziteState[schema::gridStep].toString(), track[schema::phaziteKeyScale].toString() };
    };

    // Grid levels were read as the nearest of 5..13, and anything that isn't a number as 0.
    for (const auto& level : { juce::var (3), juce::var (-4), juce::var ("abc") })
        CHECK (upgrade (level, 0).first == juce::String ("1/16 beat"));

    const char* const steps[] = { "1/16 beat", "1/8 beat", "1/4 beat", "1/2 beat", "1 beat", "1 bar", "2 bars", "4 bars", "8 bars" };

    for (int level = 5; level <= 13; ++level)
    {
        CAPTURE (level);
        const auto step = upgrade (juce::String (level), 0).first; // as text, as a file holds it
        CHECK (step == juce::String (steps[(size_t) (level - 5)]));
        CHECK (phazite::findFixedGridLevel (step) == level);
    }

    CHECK (upgrade (14, 0).first == juce::String ("8 bars"));
    CHECK (upgrade (7, 0).first == juce::String ("1/4 beat")); // a number, as a state made in memory holds it

    // Scale numbers 0..10, as the nearest of those: each name has the notes the old number had.
    const char* const scales[] = { "major",      "minor", "major",   "dorian",       "phrygian",     "lydian",
                                   "mixolydian", "minor", "locrian", "melodicMinor", "harmonicMinor" };

    for (int number = 0; number <= 10; ++number)
    {
        CAPTURE (number);
        const auto name = upgrade (5, juce::String (number)).second;
        CHECK (name == juce::String (scales[(size_t) number]));

        const auto type = phazite::findScale (name);
        REQUIRE (type.has_value());
        CHECK (te::Scale (phazite::toTracktion (*type)).getSteps() == te::Scale ((te::Scale::ScaleType) number).getSteps());
    }

    CHECK (upgrade (5, 11).second == juce::String ("harmonicMinor"));
    CHECK (upgrade (5, -1).second == juce::String ("major"));
    CHECK (upgrade (5, "x").second == juce::String ("major"));
}

TEST_CASE ("Upgrading again changes nothing")
{
    auto state = juce::ValueTree::fromXml (oldProject);
    REQUIRE (state.isValid());
    REQUIRE (phazite::migrateToCurrentSchema (state));

    const auto upgraded = state.createCopy();
    REQUIRE (phazite::migrateToCurrentSchema (state));
    CHECK (state.isEquivalentTo (upgraded));
}

TEST_CASE ("A project in the current format is left as it is")
{
    // Even values that look like the old format's.
    auto state = juce::ValueTree::fromXml ("<EDIT><PHAZITE schemaVersion=\"" + juce::String (phazite::schema::currentVersion)
                                           + R"(" gridLevel="13"/><TRACK phaziteKeyRoot="0" phaziteKeyScale="3"/></EDIT>)");
    REQUIRE (state.isValid());
    const auto before = state.createCopy();

    REQUIRE (phazite::migrateToCurrentSchema (state));
    CHECK (state.isEquivalentTo (before));

    // An invalid tree isn't a project to upgrade.
    juce::ValueTree invalid;
    CHECK (phazite::migrateToCurrentSchema (invalid));
    CHECK_FALSE (invalid.isValid());
}

TEST_CASE ("A version 1 project without notes becomes the current version with nothing else changed")
{
    // Version 2 only adds Phazite's own devices, which a version 1 project has none of, and version 3 notes' IDs.
    const auto* version1 =
        R"(<EDIT><PHAZITE schemaVersion="1" gridStep="1 bar"><LASTKEY phaziteKeyRoot="2" phaziteKeyScale="dorian"/></PHAZITE>
                              <TRACK id="1003" phaziteKeyRoot="9" phaziteKeyScale="minor"><PLUGIN type="reverb"/></TRACK></EDIT>)";
    auto state = juce::ValueTree::fromXml (version1);
    REQUIRE (state.isValid());
    CHECK (phazite::getSchemaVersion (state) == 1);

    REQUIRE (phazite::migrateToCurrentSchema (state));
    CHECK (phazite::getSchemaVersion (state) == phazite::schema::currentVersion);

    auto expected = juce::ValueTree::fromXml (version1);
    expected.getChildWithName (phazite::schema::PHAZITE)
        .setProperty (phazite::schema::schemaVersion, phazite::schema::currentVersion, nullptr);
    CHECK (state.isEquivalentTo (expected));
}

TEST_CASE ("A version 2 project becomes version 3 with an ID for each note, 1 and up in each clip")
{
    const auto* version2 = R"(<EDIT><PHAZITE schemaVersion="2"/>
        <TRACK id="1003">
          <MIDICLIP id="1004"><SEQUENCE ver="1"><NOTE p="60" b="0.0" l="1.0" v="100"/><CONTROL b="0.0" type="7" val="100"/>
                                                <NOTE p="64" b="1.0" l="1.0" v="90"/></SEQUENCE></MIDICLIP>
          <MIDICLIP id="1005"><SEQUENCE ver="1"><NOTE p="67" b="0.0" l="2.0" v="80"/></SEQUENCE></MIDICLIP>
        </TRACK></EDIT>)";
    auto state = juce::ValueTree::fromXml (version2);
    REQUIRE (state.isValid());
    REQUIRE (phazite::migrateToCurrentSchema (state));
    CHECK (phazite::getSchemaVersion (state) == 3);

    // The notes get IDs, and nothing else changes: not the controller event between them, nor the other properties.
    auto expected = juce::ValueTree::fromXml (version2);
    expected.getChildWithName (phazite::schema::PHAZITE).setProperty (phazite::schema::schemaVersion, 3, nullptr);
    const auto track = expected.getChildWithName ("TRACK");
    const auto first = track.getChild (0).getChildWithName ("SEQUENCE"), second = track.getChild (1).getChildWithName ("SEQUENCE");
    first.getChild (0).setProperty (phazite::schema::phaziteNoteID, 1, nullptr);
    first.getChild (2).setProperty (phazite::schema::phaziteNoteID, 2, nullptr);
    second.getChild (0).setProperty (phazite::schema::phaziteNoteID, 1, nullptr);
    CHECK (state.isEquivalentTo (expected));
}

TEST_CASE ("A project from a newer Phazite isn't opened or changed")
{
    const auto newer = juce::String (R"(<?xml version="1.0" encoding="UTF-8"?>
<EDIT appVersion="9.0.0" projectID="0/5eed0b2" creationTime="1791863965976" modifiedBy="">
  <PHAZITE schemaVersion="NEWER" gridStep="something new"/>
</EDIT>
)")
                           .replace ("NEWER", juce::String (phazite::schema::currentVersion + 1));

    auto state = juce::ValueTree::fromXml (newer);
    REQUIRE (state.isValid());
    const auto before = state.createCopy();
    CHECK (phazite::getSchemaVersion (state) == phazite::schema::currentVersion + 1);
    CHECK_FALSE (phazite::migrateToCurrentSchema (state));
    CHECK (state.isEquivalentTo (before));

    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("New Song").getChildFile ("New Song.phazite");
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    REQUIRE (file.replaceWithText (newer));
    const auto written = file.loadFileAsString();
    CHECK (phazite::Project::isFromNewerPhazite (phazite::test::getEngine(), file));

    juce::String error;
    const auto project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    CHECK (project == nullptr);
    CHECK (error.contains ("newer"));
    CHECK (file.loadFileAsString() == written);
}

TEST_CASE ("An old project opens upgraded, without unsaved changes or undo steps, and saves in the current format")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    juce::String error;
    juce::File file;

    // A project with a track, a MIDI clip and an alias of it...
    {
        auto created = phazite::Project::create (engine, temp.folder, "Old Song", error);
        INFO (error.toStdString());
        REQUIRE (created != nullptr);
        file = created->getFile();

        auto* track = phazite::insertTrack (created->getEdit(), nullptr, nullptr);
        REQUIRE (track != nullptr);
        auto* clip = phazite::insertMidiClip (*track, { te::BeatPosition(), te::BeatPosition::fromBeats (4.0) }, nullptr);
        REQUIRE (clip != nullptr);
        REQUIRE (phazite::copyClips ({ clip }, true).size() == 1);
        phazite::test::settle();
        REQUIRE (created->save().wasOk());
    }

    // ...rewritten as version 0 saved it: no version, a fixed 4-bar grid as a level, and a key's scale as a number.
    auto xml = phazite::test::readXml (file);
    auto* phaziteXml = xml->getChildByName ("PHAZITE");
    auto* trackXml = xml->getChildByName ("TRACK");
    REQUIRE (phaziteXml != nullptr);
    REQUIRE (trackXml != nullptr);
    phaziteXml->removeAttribute ("schemaVersion");
    phaziteXml->setAttribute ("gridAdaptive", 0);
    phaziteXml->setAttribute ("gridLevel", 12);
    trackXml->setAttribute ("phaziteKeyRoot", 2);
    trackXml->setAttribute ("phaziteKeyScale", 3);
    REQUIRE (xml->writeTo (file));
    const auto oldText = file.loadFileAsString();

    const auto checkUpgraded = [] (const phazite::Project& project)
    {
        const auto& edit = project.getEdit();
        const auto key = phazite::getTrackKey (*te::getAudioTracks (edit).getFirst());
        REQUIRE (key.has_value());
        CHECK (key->root == 2);
        CHECK (key->scale == phazite::Scale::dorian);
        CHECK (phazite::loadGridSettings (edit, phazite::GridOwner::playlist).fixedLevel == 12);
    };

    const auto checkSavedInCurrentFormat = [&file]
    {
        const auto saved = phazite::test::readXml (file);
        const auto* savedPhazite = saved->getChildByName ("PHAZITE");
        REQUIRE (savedPhazite != nullptr);
        CHECK (savedPhazite->getIntAttribute ("schemaVersion") == phazite::schema::currentVersion);
        CHECK (savedPhazite->getStringAttribute ("gridStep") == juce::String ("4 bars"));
        CHECK_FALSE (savedPhazite->hasAttribute ("gridLevel"));
        CHECK (saved->getChildByName ("TRACK")->getStringAttribute ("phaziteKeyScale") == juce::String ("dorian"));
    };

    // It opens upgraded, as the same song, with nothing to save or undo; the file stays as it was until a save.
    {
        auto project = openProject (file); // settles
        checkUpgraded (*project);
        CHECK_FALSE (project->hasUnsavedChanges());
        CHECK_FALSE (project->getEdit().getUndoManager().canUndo());
        CHECK (file.loadFileAsString() == oldText);

        // Saved, it is in the current format, and the old file is kept in Backups.
        REQUIRE (project->save().wasOk());
        checkSavedInCurrentFormat();
        const auto backups = project->getBackupsFolder().findChildFiles (juce::File::findFiles, false, "*.phazite");
        CHECK (std::any_of (backups.begin(), backups.end(),
                            [] (const juce::File& backup) { return backup.loadFileAsString().contains ("gridLevel=\"12\""); }));
    }

    // The same from a version-0 autosave, which opens with unsaved changes on purpose: they aren't in the project file.
    REQUIRE (phazite::Project::getAutosaveFile (file).replaceWithText (oldText));
    auto restored = phazite::Project::open (engine, file, true, error);
    INFO (error.toStdString());
    REQUIRE (restored != nullptr);
    checkUpgraded (*restored);
    CHECK (restored->hasUnsavedChanges());
    REQUIRE (restored->save().wasOk());
    checkSavedInCurrentFormat();
}
