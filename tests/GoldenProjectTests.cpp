// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden files of saved projects (tests/golden/projects): a new project, the First Beat example song and a project with one of
// each kind of track and every piece of Phazite's data are saved, reopened and saved again, and the second save is
// compared with a committed file. GoldenFiles.h says what the comparison leaves out and how to regenerate the files.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "engine/nodes/Channel.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/chords/Progressions.h"
#include "model/chords/internal/Chords.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "ui/pianoroll/DrumRows.h"

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

std::unique_ptr<phazite::Project> createProject (const juce::File& parentFolder, const juce::String& name)
{
    juce::String error;
    auto project = phazite::Project::create (parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

std::unique_ptr<phazite::Project> openProject (const juce::File& file)
{
    juce::String error;
    auto project = phazite::Project::open (file, false, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    phazite::test::settle();
    return project;
}

juce::String getAttributeNames (const juce::XmlElement& element)
{
    juce::StringArray names;

    for (const auto& attribute : element.getAttributeIterator())
        names.add (attribute.name.toString());

    return names.joinIntoString (" ");
}

juce::String getChildTags (const juce::XmlElement& element)
{
    juce::StringArray tags;

    for (const auto* child : element.getChildIterator())
        tags.add (child->getTagName());

    return tags.joinIntoString (" ");
}

//==============================================================================
// Every kind of track and every piece of Phazite's data that a project keeps, so that its golden file covers them all.
void addRepresentativeContent (phazite::Project& project, const juce::File& scratch)
{
    using phazite::test::makeBuiltInDevice;

    auto& song = project.getSong();
    phazite::setTempo (song, 120.0);
    phazite::setTimeSignature (song, { 4, 4 });

    const auto drums = phazite::getTrackIDs (song).front();
    phazite::setTrackName (song, drums, "Drums");

    // Sections: two from an outline, and one of their own.
    const auto outline = phazite::Outline::fromJSON (R"({ "id": "golden", "name": "Golden", "sections": [
        { "name": "Intro", "role": "intro", "bars": 2, "energy": 0.25, "tip": "Set the mood." },
        { "name": "Drop", "role": "drop", "bars": 4, "energy": 0.9, "colour": "Yellow", "tip": "Everything at once." } ] })");
    REQUIRE (outline.has_value());
    phazite::applyOutline (song, *outline);
    const phazite::SectionInfo outro { .name = "Outro",
                                       .length = phazite::BeatDuration::fromBeats (8.0),
                                       .role = "outro",
                                       .tip = "Let it fade.",
                                       .energy = 0.1f };
    REQUIRE (phazite::insertSection (song, phazite::BeatPosition::fromBeats (24.0), outro).isValid());

    // Drums: a beat, shown in drum mode with the snare's row first and a row hidden; the track pinned and muted.
    const auto beat = phazite::insertMidiClip (song, drums, { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (beat.isValid());

    for (const auto start : { 0.0, 2.0 })
        phazite::addNote (song, beat, 36, beats (start), phazite::BeatDuration::fromBeats (0.25), 110);

    for (const auto start : { 1.0, 3.0 })
        phazite::addNote (song, beat, 38, beats (start), phazite::BeatDuration::fromBeats (0.25), 110);

    phazite::setDrumTrack (song, drums, true);
    const auto drumRows = phazite::ui::getDrumRows (phazite::getSamplerSounds (song, drums), phazite::getNoteNumbersPlayed (song, { beat }),
                                                    phazite::getDrumRowOrder (song, drums), phazite::getHiddenDrumRows (song, drums));
    phazite::setDrumRowOrder (song, drums, phazite::ui::getDrumOrderAfterMove (drumRows, 38, 0)); // the snare's row dragged to the top
    phazite::hideDrumRow (song, drums, 49);
    phazite::setPinned (song, drums, true);
    phazite::setTrackMuted (song, drums, true);

    // Keys: an instrument, and an effect with a mix of its own; a key; chords with a muted note, and an alias of them;
    // a height of its own, and a volume curve.
    const auto keys = phazite::insertTrack (song, drums, nullptr);
    REQUIRE (keys.isValid());
    phazite::setTrackName (song, keys, "Keys");
    REQUIRE (phazite::setInstrument (song, keys, makeBuiltInDevice (song, phazite::devices::Synth::typeID)).isValid());
    const auto reverb = phazite::insertDeviceState (song, keys, makeBuiltInDevice (song, phazite::devices::Reverb::typeID));
    REQUIRE (reverb.isValid());
    phazite::setMix (song, reverb, 0.25f);
    phazite::setTrackKey (song, keys, phazite::Key { 2, phazite::Scale::minor });

    const auto chords = phazite::insertMidiClip (song, keys, { beats (0.0), beats (8.0) }, nullptr);
    REQUIRE (chords.isValid());
    phazite::addNote (song, chords, 62, beats (0.0), phazite::BeatDuration::fromBeats (2.0), 90);
    const auto quiet = phazite::addNote (song, chords, 65, beats (2.0), phazite::BeatDuration::fromBeats (1.5), 70);
    REQUIRE (quiet.isValid());
    phazite::toggleNotesMuted (song, { quiet });

    const auto alias = phazite::copyClips (song, { chords }, true);
    REQUIRE (alias.size() == 1);
    phazite::moveClip (song, alias[0], beats (8.0), keys);
    phazite::setSavedTrackHeight (song, keys, 96);

    const phazite::ParameterRef keysVolume { phazite::getFader (song, keys), phazite::getVolumeParameterID (song, keys) };
    REQUIRE (keysVolume.isValid());
    phazite::addPoint (song, keysVolume, { phazite::BeatPosition(), phazite::engine::decibelsToFaderPosition (0.0f), 0.0f });
    phazite::addPoint (song, keysVolume,
                       { phazite::BeatPosition::fromBeats (8.0), phazite::engine::decibelsToFaderPosition (-6.0f), 0.0f });

    // Pad: a new track, which gets the key set last.
    const auto pad = phazite::insertTrack (song, keys, nullptr);
    REQUIRE (pad.isValid());
    phazite::setTrackName (song, pad, "Pad");

    // Tone: imported audio with fades of a shape of their own, on a track panned left. (No digits in the
    // file's name: Phazite would read a tempo in them and stretch the clip.)
    const auto tone = phazite::insertTrack (song, pad, nullptr);
    REQUIRE (tone.isValid());
    phazite::setTrackName (song, tone, "Tone");
    const auto audio = phazite::importFile (song, tone, phazite::test::writeSound (scratch.getChildFile ("Tone.wav"), 2.0), beats (4.0));
    REQUIRE (phazite::hasAudioClips (song, { audio }));
    phazite::setClipFade (song, audio, true, 0.25);
    phazite::setClipFade (song, audio, false, 0.5);
    phazite::setFadeShape (song, { audio }, phazite::convexFade);
    phazite::setChannelPan (song, tone, -0.5f);

    // Folders: Keys and Pad in a collapsed folder, and Tone in a bus at -3 dB.
    const auto group = phazite::groupIntoFolder (song, { keys, pad }, nullptr);
    REQUIRE (group.isValid());
    phazite::setTrackName (song, group, "Group");
    phazite::setCollapsed (song, group, true);

    const auto bus = phazite::groupIntoFolder (song, { tone }, nullptr);
    REQUIRE (bus.isValid());
    phazite::setTrackName (song, bus, "Bus");
    phazite::setMixedAsBus (song, bus, true);
    phazite::setChannelVolume (song, bus, -3.0f);

    // Returns: a reverb return that Tone sends to after its fader and Keys before its fader; and a reverb on the master.
    const auto reverbReturn = phazite::addReturnWithEffect (song, tone, makeBuiltInDevice (song, phazite::devices::Reverb::typeID));
    REQUIRE (reverbReturn.isValid());
    REQUIRE (phazite::addSend (song, keys, reverbReturn, true).isValid());
    REQUIRE (phazite::insertDeviceState (song, phazite::getMasterID (song), makeBuiltInDevice (song, phazite::devices::Reverb::typeID))
                 .isValid());

    // View settings: the playlist's grid and the piano roll's, and a range looped on purpose.
    phazite::saveGridSettings (song, phazite::GridOwner::playlist, { .snap = false, .adaptiveOffset = 1 });
    phazite::saveGridSettings (song, phazite::GridOwner::pianoRoll, { .adaptive = false, .fixedLevel = 8, .triplets = true });

    phazite::loopBeats (song, { beats (4.0), beats (12.0) });

    // The chord lane: a progression in D minor over the intro and the drop, with a borrowed chord in its second inversion,
    // a key change to F major for the outro, and a bass line made from the chords that follows them on a track of its own.
    const auto progression = phazite::Progression::fromJSON (
        juce::JSON::parse (R"({ "id": "golden", "name": "Golden", "scale": "minor", "chords": "i*2 VI VII/1 iim7b5 V7" })"));
    REQUIRE (progression.has_value());
    const auto at = [] (double b) { return phazite::BeatPosition::fromBeats (b); };
    phazite::setLaneKey (song, at (0.0), { 2, phazite::Scale::minor });
    phazite::putProgression (song, *progression, { at (0.0), at (24.0) });
    phazite::putLaneChord (song, { at (20.0), at (24.0) }, { 2, -1, phazite::ChordType::majorSeventh, 2 });
    phazite::setLaneKey (song, at (24.0), { 5, phazite::Scale::major });
    phazite::putLaneChord (song, { at (24.0), at (32.0) }, { 1, 0, phazite::ChordType::suspendedFourth, 0 });
    REQUIRE (phazite::addChordPart (song, phazite::PartStyle::bassRootFifth, { at (0.0), at (32.0) }, nullptr).isValid());
}

//==============================================================================
// The saves each golden project test compares: S1, the project as made; S2, saved after reopening S1; S3, saved after
// reopening S2.
struct Saves
{
    std::unique_ptr<juce::XmlElement> rawFirst;  // S1 as Phazite wrote it
    std::unique_ptr<juce::XmlElement> first;     // S1, normalised
    std::unique_ptr<juce::XmlElement> rawSecond; // S2 as Phazite wrote it
};

// Saves a project (S1), reopens and saves it (S2), and does that again (S3). S2 holds everything S1 does (opening it may
// add what every song has), S3 is S2, and S2 matches the golden file tests/golden/projects/<goldenName>.xml. Each
// reopened project has no unsaved changes once the message loop has run after it opened (openProject() lets it):
// opening changes nothing.
Saves saveReopenAndSaveAgain (std::unique_ptr<phazite::Project> project, const juce::String& goldenName, const juce::String& about)
{
    const auto file = project->getFile();
    Saves saves;

    REQUIRE (project->save().wasOk());
    saves.rawFirst = phazite::test::readXml (file);
    saves.first = phazite::test::normaliseProject (*saves.rawFirst, file.getParentDirectory());
    project = nullptr;

    project = openProject (file);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());
    saves.rawSecond = phazite::test::readXml (file);
    const auto second = phazite::test::normaliseProject (*saves.rawSecond, file.getParentDirectory());
    project = nullptr;

    project = openProject (file);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());
    const auto third = phazite::test::readNormalisedProject (file);
    project = nullptr;

    phazite::test::checkSameProject ("reopened and saved", *saves.first, *second, { .actualMayAdd = true });
    phazite::test::checkSameProject ("saved again", *second, *third);
    phazite::test::checkProjectGolden ("projects/" + goldenName + ".xml", *second, about);
    return saves;
}
} // namespace

//==============================================================================
TEST_SUITE ("golden")
{
    TEST_CASE ("Golden files: the project normaliser removes what changes from run to run, and nothing else")
    {
        // A save with all that the normaliser changes: a clip and its alias (one with a Windows path), float noise, -0,
        // unsorted attributes and children, and empty elements, one of them Phazite's.
        auto saved = juce::parseXML (
            R"(<PHAZITE outline="pop" nextID="1012">
  <TRACKS>
    <TRACK name="Lead" id="1004">
      <CLIP source="Audio\Tone.wav" name="Tone" id="1010" linkID="{6f0c51e2-3b4a-4d7e-9c1f-2a8b7d6e5f40}" length="0.6000000238418579"/>
      <SCRATCH/>
    </TRACK>
    <TRACK name="Bass" id="1003" pan="-0.0">
      <CLIP name="Tone" id="1011" linkID="{6f0c51e2-3b4a-4d7e-9c1f-2a8b7d6e5f40}" length="1.5"/>
    </TRACK>
  </TRACKS>
  <PIANOROLL/>
</PHAZITE>)");
        REQUIRE (saved != nullptr);

        // The alias's audio, at an absolute path in the project's folder.
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Normalised Song");
        auto* savedTracks = saved->getChildByName ("TRACKS");
        REQUIRE (savedTracks != nullptr);
        auto* savedBass = savedTracks->getChildByAttribute ("id", "1003");
        REQUIRE (savedBass != nullptr);
        auto* savedAlias = savedBass->getChildByName ("CLIP");
        REQUIRE (savedAlias != nullptr);
        savedAlias->setAttribute ("source", folder.getChildFile ("Audio").getChildFile ("Tone.wav").getFullPathName());

        const auto normalised = phazite::test::normaliseProject (*saved, folder, { "SCRATCH" });
        REQUIRE (normalised != nullptr);

        // The root's attributes are sorted, and its children by their tags; the tracks keep their order.
        CHECK (getAttributeNames (*normalised) == juce::String ("nextID outline"));
        CHECK (normalised->getStringAttribute ("nextID") == juce::String ("1012"));
        CHECK (getChildTags (*normalised) == juce::String ("PIANOROLL TRACKS"));
        auto* tracks = normalised->getChildByName ("TRACKS");
        REQUIRE (tracks != nullptr);
        CHECK (getChildTags (*tracks) == juce::String ("TRACK TRACK"));
        auto* lead = tracks->getChildElement (0);
        auto* bass = tracks->getChildElement (1);
        REQUIRE (lead != nullptr);
        REQUIRE (bass != nullptr);
        CHECK (lead->getStringAttribute ("id") == juce::String ("1004"));
        CHECK (bass->getStringAttribute ("id") == juce::String ("1003"));
        CHECK (bass->getStringAttribute ("pan") == juce::String ("0"));

        // The clip and its alias share link-1; paths have '/' and no project folder; the float noise is gone, and so is
        // the empty SCRATCH.
        CHECK (getChildTags (*lead) == juce::String ("CLIP"));
        auto* clip = lead->getChildByName ("CLIP");
        auto* alias = bass->getChildByName ("CLIP");
        REQUIRE (clip != nullptr);
        REQUIRE (alias != nullptr);
        CHECK (getAttributeNames (*clip) == juce::String ("id length linkID name source"));
        CHECK (clip->getStringAttribute ("linkID") == juce::String ("link-1"));
        CHECK (alias->getStringAttribute ("linkID") == juce::String ("link-1"));
        CHECK (clip->getStringAttribute ("source") == juce::String ("Audio/Tone.wav"));
        CHECK (alias->getStringAttribute ("source") == juce::String ("$PROJECT/Audio/Tone.wav"));
        CHECK (clip->getStringAttribute ("length") == juce::String ("0.6"));
        CHECK (alias->getStringAttribute ("length") == juce::String ("1.5"));

        // Phazite's empty PIANOROLL stays: if it vanished, a read that adds an empty one would go unnoticed.
        auto* pianoRoll = normalised->getChildByName ("PIANOROLL");
        REQUIRE (pianoRoll != nullptr);
        CHECK (pianoRoll->getNumAttributes() == 0);

        for (const auto* tag : { "PHAZITE", "PIANOROLL", "LASTKEY" })
            CHECK_FALSE (phazite::test::getDroppableEmptyTags().contains (tag));

        // The comparison: float noise beyond 7 digits isn't a difference; a changed value is one, with its path; an
        // attribute only the actual one has is one unless it may add some.
        const auto expected = juce::parseXML (R"(<PHAZITE><TRACK id="1003" height="64" volume="0.6376281"/></PHAZITE>)");
        const auto noisy = juce::parseXML (R"(<PHAZITE><TRACK id="1003" height="64" volume="0.6376282"/></PHAZITE>)");
        const auto taller = juce::parseXML (R"(<PHAZITE><TRACK id="1003" height="96" volume="0.6376281"/></PHAZITE>)");
        const auto added = juce::parseXML (R"(<PHAZITE><TRACK id="1003" height="64" volume="0.6376281" mute="1"/></PHAZITE>)");
        REQUIRE (expected != nullptr);
        REQUIRE (noisy != nullptr);
        REQUIRE (taller != nullptr);
        REQUIRE (added != nullptr);

        CHECK (phazite::test::findDifferences (*expected, *noisy).isEmpty());

        const auto heightChange = phazite::test::findDifferences (*expected, *taller);
        REQUIRE (heightChange.size() == 1);
        CHECK (heightChange[0].startsWith ("PHAZITE/TRACK#1003@height: "));

        CHECK (phazite::test::findDifferences (*expected, *added).size() == 1);
        CHECK (phazite::test::findDifferences (*expected, *added, { .actualMayAdd = true }).isEmpty());
    }

    TEST_CASE ("Golden files: a name counts in a save only as a whole word in a value, ignoring case")
    {
        const auto save = juce::parseXML (R"(<PHAZITE nextID="1011" savedTime="1791863965976" savedBy="Tim Smith">
  <TEMPOMAP>
    <TIMESIGNATURE numerator="4" denominator="4" beat="0"/>
  </TEMPOMAP>
  <TRACKS>
    <TRACK id="1003" name="Timeline">
      <CLIP id="1010" name="Tim's take" file="C:\Users\tim\Music\Tone.wav"/>
      <NOTES>written by TIM</NOTES>
    </TRACK>
    <TRACK id="1004" name="Jos&#233;"/>
    <TRACK id="1005" name="/home/dev/Sounds"/>
  </TRACKS>
</PHAZITE>)");
        REQUIRE (save != nullptr);

        // In values, at every depth, as a whole word whatever its case; not in tags or attribute names (TIMESIGNATURE,
        // savedTime), nor inside a longer word (Timeline).
        const auto tim = phazite::test::findValuesNaming (*save, { "tim" });
        REQUIRE (tim.size() == 4);
        CHECK (tim[0] == juce::String (R"(PHAZITE@savedBy: "Tim Smith")"));
        CHECK (tim[1] == juce::String (R"(PHAZITE/TRACKS/TRACK#1003/CLIP#1010@name: "Tim's take")"));
        CHECK (tim[2] == juce::String (R"(PHAZITE/TRACKS/TRACK#1003/CLIP#1010@file: "C:\Users\tim\Music\Tone.wav")"));
        CHECK (tim[3] == juce::String (R"(PHAZITE/TRACKS/TRACK#1003/NOTES/#text: "written by TIM")"));

        CHECK (phazite::test::findValuesNaming (*save, { "Tim Smith" }).joinIntoString ("\n")
               == juce::String (R"(PHAZITE@savedBy: "Tim Smith")"));
        CHECK (phazite::test::findValuesNaming (*save, { "dev" }).joinIntoString ("\n")
               == juce::String (R"(PHAZITE/TRACKS/TRACK#1005@name: "/home/dev/Sounds")"));

        // A letter written as a character reference counts as itself.
        const auto jose = juce::String (juce::CharPointer_UTF8 ("Jos\xc3\xa9"));
        CHECK (phazite::test::findValuesNaming (*save, juce::StringArray { jose }).joinIntoString ("\n")
               == "PHAZITE/TRACKS/TRACK#1004@name: \"" + jose + "\"");

        CHECK (phazite::test::findValuesNaming (*save, { "TIMESIGNATURE", "savedTime", "devices", "line", "Smit" }).isEmpty());
    }

    TEST_CASE ("Golden project: a new, empty project")
    {
        phazite::test::TemporaryFolder temp;
        auto project = createProject (temp.folder, "Empty");
        phazite::test::settle();

        saveReopenAndSaveAgain (std::move (project), "empty",
                                "A new, empty project (Project::create), saved, reopened and saved again: tests/GoldenProjectTests.cpp");
    }

    TEST_CASE ("Golden project: the First Beat example song")
    {
        // As Help > Example Songs makes it (ProjectSession::openExampleSong()).
        phazite::test::TemporaryFolder temp;
        juce::String error;
        auto project = phazite::Project::create (
            temp.folder, "First Beat", error,
            phazite::getExampleSongContent (phazite::test::getEngineContext(), "firstBeat", phazite::test::getFactoryPresetsFolder()));
        INFO (error.toStdString());
        REQUIRE (project != nullptr);
        phazite::test::settle();

        // The first start shows no unsaved changes, and nothing to undo.
        CHECK_FALSE (project->hasUnsavedChanges());
        CHECK_FALSE (project->getSong().getUndoManager().canUndo());

        saveReopenAndSaveAgain (std::move (project), "first-beat",
                                "The First Beat example song (ExampleSongs.h), saved, reopened and saved again: "
                                "tests/GoldenProjectTests.cpp");
    }

    TEST_CASE ("Golden project: one of each kind of track and every piece of Phazite data")
    {
        phazite::test::TemporaryFolder temp;
        auto project = createProject (temp.folder, "Representative");
        addRepresentativeContent (*project, temp.folder.getChildFile ("Sounds"));
        phazite::test::settle();

        // The autosave, and a copy saved as another project, are compared with the project's file below.
        REQUIRE (project->autosave().wasOk());
        const auto autosave = phazite::test::normaliseProject (
            *phazite::test::readXml (phazite::Project::getAutosaveFile (project->getFile())), project->getFolder());

        juce::String error;
        const auto copyFile = project->saveCopyAs (temp.folder, "Representative Copy", error);
        REQUIRE_MESSAGE (copyFile.existsAsFile(), error.toStdString());
        const auto copy = phazite::test::readNormalisedProject (copyFile);

        const auto saves = saveReopenAndSaveAgain (std::move (project), "representative",
                                                   "A project with one of each kind of track and every piece of Phazite's data, saved, "
                                                   "reopened and saved again: addRepresentativeContent in tests/GoldenProjectTests.cpp");

        // Nothing in it names the person who made it.
        phazite::test::checkNoUserName ("The first save", *saves.rawFirst);

        phazite::test::checkSameProject ("in the autosave", *saves.first, *autosave);
        phazite::test::checkSameProject ("saved as a copy", *saves.first, *copy);
    }
}
