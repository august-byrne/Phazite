// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden files of saved projects (tests/golden/projects): a new project, the starter project and a project with one of
// each kind of track and every piece of Phazite's data are saved, reopened and saved again, and the second save is
// compared with a committed file. The representative project also freezes the 0.1.0 project that MigrationTests.cpp
// opens. GoldenFiles.h says what the comparison leaves out and how to regenerate the files.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Grid.h"
#include "model/arrangement/Import.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/Sections.h"
#include "model/arrangement/Tracks.h"
#include "model/mixer/Mixer.h"
#include "model/notes/Notes.h"
#include "model/project/Project.h"
#include "model/project/StarterProject.h"
#include "model/transport/Playback.h"

using namespace tracktion::literals;

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

std::unique_ptr<phazite::Project> createProject (const juce::File& parentFolder, const juce::String& name)
{
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

std::unique_ptr<phazite::Project> openProject (const juce::File& file)
{
    juce::String error;
    auto project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
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

// Every value in a saved project, one per line: its attributes' values and its text. Only these could name the person
// who saved it (tags and attribute names are Tracktion's and Phazite's own). They are read from the XML, so a letter
// that JUCE writes as a character reference (every letter outside ASCII, such as &#233;) is here as itself.
juce::String getAllValues (const juce::XmlElement& element)
{
    if (element.isTextElement())
        return element.getText() + "\n";

    juce::String values;

    for (const auto& attribute : element.getAttributeIterator())
        values << attribute.value << "\n";

    for (const auto* child : element.getChildIterator())
        values << getAllValues (*child);

    return values;
}

//==============================================================================
// Every kind of track and every piece of Phazite's data that a project keeps, so that its golden file covers them all.
void addRepresentativeContent (phazite::Project& project, const juce::File& scratch)
{
    using phazite::test::createBuiltInDevice;

    auto& edit = project.getEdit();
    edit.tempoSequence.getTempos()[0]->setBpm (120.0);
    edit.tempoSequence.getTimeSigs()[0]->setStringTimeSig ("4/4");

    auto* drums = te::getAudioTracks (edit)[0];
    REQUIRE (drums != nullptr);
    drums->setName ("Drums");

    // Sections: two from an outline, and one of their own.
    const auto outline = phazite::Outline::fromJSON (R"({ "id": "golden", "name": "Golden", "sections": [
        { "name": "Intro", "role": "intro", "bars": 2, "energy": 0.25, "tip": "Set the mood." },
        { "name": "Drop", "role": "drop", "bars": 4, "energy": 0.9, "colour": "Yellow", "tip": "Everything at once." } ] })");
    REQUIRE (outline.has_value());
    phazite::applyOutline (edit, *outline);
    const phazite::SectionInfo outro { .name = "Outro",
                                       .length = te::BeatDuration::fromBeats (8.0),
                                       .role = "outro",
                                       .tip = "Let it fade.",
                                       .energy = 0.1f };
    REQUIRE (phazite::insertSection (edit, beats (24.0), outro) != nullptr);

    // Drums: a beat, shown in drum mode with the snare's row first and a row hidden; the track pinned and muted.
    auto* beat = phazite::insertMidiClip (*drums, { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (beat != nullptr);

    for (const auto start : { 0.0, 2.0 })
        phazite::addNote (*beat, 36, beats (start), te::BeatDuration::fromBeats (0.25), 110);

    for (const auto start : { 1.0, 3.0 })
        phazite::addNote (*beat, 38, beats (start), te::BeatDuration::fromBeats (0.25), 110);

    phazite::setDrumTrack (*drums, true);
    phazite::moveDrumRow (*drums, phazite::getDrumRows (*drums, { beat }), 38, 0);
    phazite::hideDrumRow (*drums, 49);
    phazite::setPinned (*drums, true);
    drums->setMute (true);

    // Keys: an instrument, and an effect with a mix of its own; a key; chords with a muted note, and an alias of them;
    // a height of its own, and a volume curve.
    auto* keys = phazite::insertTrack (edit, drums, nullptr);
    REQUIRE (keys != nullptr);
    keys->setName ("Keys");
    REQUIRE (phazite::setInstrument (*keys, createBuiltInDevice (edit, te::FourOscPlugin::xmlTypeName)) != nullptr);
    auto* reverb = phazite::insertDevice (*keys, createBuiltInDevice (edit, te::ReverbPlugin::xmlTypeName));
    REQUIRE (reverb != nullptr);
    phazite::setMix (*reverb, 0.25f);
    phazite::setTrackKey (*keys, phazite::Key { 2, te::Scale::minor });

    auto* chords = phazite::insertMidiClip (*keys, { beats (0.0), beats (8.0) }, nullptr);
    REQUIRE (chords != nullptr);
    phazite::addNote (*chords, 62, beats (0.0), te::BeatDuration::fromBeats (2.0), 90);
    auto* quiet = phazite::addNote (*chords, 65, beats (2.0), te::BeatDuration::fromBeats (1.5), 70);
    REQUIRE (quiet != nullptr);
    phazite::toggleNotesMuted ({ { chords, quiet } });

    const auto alias = phazite::copyClips ({ chords }, true);
    REQUIRE (alias.size() == 1);
    phazite::moveClip (*alias[0], beats (8.0), *keys);
    phazite::setTrackHeight (*keys, 96);

    auto* keysVolume = keys->getVolumePlugin();
    REQUIRE (keysVolume != nullptr);
    auto& volumeCurve = keysVolume->volParam->getCurve();
    volumeCurve.addPoint (0_tp, te::decibelsToVolumeFaderPosition (0.0f), 0.0f, nullptr);
    volumeCurve.addPoint (4_tp, te::decibelsToVolumeFaderPosition (-6.0f), 0.0f, nullptr);

    // Pad: a new track, which gets the key set last.
    auto* pad = phazite::insertTrack (edit, keys, nullptr);
    REQUIRE (pad != nullptr);
    pad->setName ("Pad");

    // Tone: imported audio with fades of a shape and a color of its own, on a track panned left. (No digits in the
    // file's name: Phazite would read a tempo in them and stretch the clip.)
    auto* tone = phazite::insertTrack (edit, pad, nullptr);
    REQUIRE (tone != nullptr);
    tone->setName ("Tone");
    auto* audio = dynamic_cast<te::WaveAudioClip*> (
        phazite::importFile (*tone, phazite::test::writeSound (scratch.getChildFile ("Tone.wav"), 2.0), beats (4.0)));
    REQUIRE (audio != nullptr);
    phazite::setClipFade (*audio, true, te::TimeDuration::fromSeconds (0.25));
    phazite::setClipFade (*audio, false, te::TimeDuration::fromSeconds (0.5));
    phazite::setFadeShape ({ audio }, te::AudioFadeCurve::convex);
    audio->setColour (phazite::getTrackColours()[4].colour);
    auto* toneVolume = tone->getVolumePlugin();
    REQUIRE (toneVolume != nullptr);
    toneVolume->setPan (-0.5f);

    // Folders: Keys and Pad in a collapsed folder, and Tone in a bus at -3 dB.
    auto* group = phazite::groupIntoFolder ({ keys, pad }, nullptr);
    REQUIRE (group != nullptr);
    group->setName ("Group");
    phazite::setCollapsed (*group, true);

    auto* bus = phazite::groupIntoFolder ({ tone }, nullptr);
    REQUIRE (bus != nullptr);
    bus->setName ("Bus");
    phazite::setMixedAsBus (*bus, true);
    auto* busVolume = dynamic_cast<te::VolumeAndPanPlugin*> (phazite::getVolumeDevice (*bus));
    REQUIRE (busVolume != nullptr);
    busVolume->setVolumeDb (-3.0f);

    // Returns: a reverb return that Tone sends to after its fader and Keys before its fader; and a reverb on the master.
    auto* reverbReturn = phazite::addReturnWithEffect (*tone, createBuiltInDevice (edit, te::ReverbPlugin::xmlTypeName));
    REQUIRE (reverbReturn != nullptr);
    REQUIRE (phazite::addSend (*keys, phazite::getReturnBus (*reverbReturn), true) != nullptr);
    REQUIRE (phazite::insertMasterDevice (edit, createBuiltInDevice (edit, te::ReverbPlugin::xmlTypeName)) != nullptr);

    // View settings: the playlist's grid and the piano roll's, and a range looped on purpose.
    auto state = project.getPhaziteState();
    const phazite::GridSettings playlistGrid { .snap = false, .adaptiveOffset = 1 };
    playlistGrid.save (state);

    auto pianoRollState = state.getOrCreateChildWithName ("PIANOROLL", nullptr);
    const phazite::GridSettings pianoRollGrid { .adaptive = false, .fixedLevel = 8, .triplets = true };
    pianoRollGrid.save (pianoRollState);

    phazite::loopRange (edit, { te::TimePosition::fromSeconds (2.0), te::TimePosition::fromSeconds (6.0) });
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

// Saves a project (S1), reopens and saves it (S2), and does that again (S3). S2 holds everything S1 does (Tracktion may
// add values it fills in on loading), S3 is S2, and S2 matches the golden file tests/golden/projects/<goldenName>.xml.
// If opensUnchanged, each reopened project has no unsaved changes.
//
// opensUnchanged is false for a project with a MIDI clip: it has unsaved changes as soon as its messages are delivered,
// so that isn't checked for it yet. Loading the Edit triggers its tempo sequence's async update, which calls every
// clip's pitchTempoTrackChanged(); a MIDI clip's sends a property-change message for "mute" without changing it
// (Tracktion's MIDI editor redraws on it), and Project::markChanged counts that as a change. The same message makes
// aliases copy each other's notes again (Clip::changed), removing and re-adding them. Tracktion's own flag
// (te::Edit::hasChangedSinceSaved) stays clear. Once that is fixed, every project here opens unchanged.
Saves saveReopenAndSaveAgain (std::unique_ptr<phazite::Project> project, const juce::String& goldenName, bool opensUnchanged,
                              const juce::String& about)
{
    const auto file = project->getFile();
    Saves saves;

    REQUIRE (project->save().wasOk());
    saves.rawFirst = phazite::test::readXml (file);
    saves.first = phazite::test::normaliseProject (*saves.rawFirst, file.getParentDirectory());
    project = nullptr;

    project = openProject (file);

    if (opensUnchanged)
        CHECK_FALSE (project->hasUnsavedChanges());

    REQUIRE (project->save().wasOk());
    saves.rawSecond = phazite::test::readXml (file);
    const auto second = phazite::test::normaliseProject (*saves.rawSecond, file.getParentDirectory());
    project = nullptr;

    project = openProject (file);

    if (opensUnchanged)
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
        // A save with all that the normaliser changes: the root's times and IDs, the machine's inputs, a clip and its
        // alias (one with a Windows path), float noise, -0, unsorted attributes and children, and empty elements, one of
        // them Phazite's.
        auto saved = juce::parseXML (
            R"(<EDIT projectID="0/5eed0b1" appVersion="0.1.0" modifiedBy="" creationTime="1791863965976" lastSignificantChange="19a8e5c0d58">
  <TRACK name="Lead" id="1004">
    <AUDIOCLIP source="Audio\Tone.wav" name="Tone" id="1010" linkID="{6f0c51e2-3b4a-4d7e-9c1f-2a8b7d6e5f40}" length="0.6000000238418579"/>
    <MODIFIERS/>
  </TRACK>
  <INPUTDEVICES>
    <INPUTDEVICE name="MIDI Keyboard" targetIndex="0"/>
  </INPUTDEVICES>
  <TRACK name="Bass" id="1003" pan="-0.0">
    <AUDIOCLIP name="Tone" id="1011" linkID="{6f0c51e2-3b4a-4d7e-9c1f-2a8b7d6e5f40}" length="1.5"/>
  </TRACK>
  <PHAZITE>
    <PIANOROLL/>
  </PHAZITE>
</EDIT>)");
        REQUIRE (saved != nullptr);

        // The alias's audio, at an absolute path in the project's folder.
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Normalised Song");
        auto* savedBass = saved->getChildByAttribute ("id", "1003");
        REQUIRE (savedBass != nullptr);
        auto* savedAlias = savedBass->getChildByName ("AUDIOCLIP");
        REQUIRE (savedAlias != nullptr);
        savedAlias->setAttribute ("source", folder.getChildFile ("Audio").getChildFile ("Tone.wav").getFullPathName());

        const auto normalised = phazite::test::normaliseProject (*saved, folder, { "INPUTDEVICES", "MODIFIERS" });
        REQUIRE (normalised != nullptr);

        // The root: what varies from run to run is marked, the time of the last change is gone, and the user's empty
        // name stays. Attributes are sorted.
        CHECK (getAttributeNames (*normalised) == juce::String ("appVersion creationTime modifiedBy projectID"));

        for (const auto* varying : { "appVersion", "creationTime", "projectID" })
            CHECK (normalised->getStringAttribute (varying) == juce::String ("(varies)"));

        CHECK (normalised->getStringAttribute ("modifiedBy", "(missing)").isEmpty());

        // Phazite's state comes before the tracks, which keep their order; the machine's inputs are gone.
        CHECK (getChildTags (*normalised) == juce::String ("PHAZITE TRACK TRACK"));
        auto* lead = normalised->getChildElement (1);
        auto* bass = normalised->getChildElement (2);
        REQUIRE (lead != nullptr);
        REQUIRE (bass != nullptr);
        CHECK (lead->getStringAttribute ("id") == juce::String ("1004"));
        CHECK (bass->getStringAttribute ("id") == juce::String ("1003"));
        CHECK (bass->getStringAttribute ("pan") == juce::String ("0"));

        // The clip and its alias share link-1; paths have '/' and no project folder; the float noise is gone, and so is
        // the empty MODIFIERS.
        CHECK (getChildTags (*lead) == juce::String ("AUDIOCLIP"));
        auto* clip = lead->getChildByName ("AUDIOCLIP");
        auto* alias = bass->getChildByName ("AUDIOCLIP");
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
        auto* phaziteState = normalised->getChildByName ("PHAZITE");
        REQUIRE (phaziteState != nullptr);
        CHECK (getChildTags (*phaziteState) == juce::String ("PIANOROLL"));

        for (const auto* tag : { "PHAZITE", "PIANOROLL", "LASTKEY" })
            CHECK_FALSE (phazite::test::getDroppableEmptyTags().contains (tag));

        // The comparison: float noise beyond 7 digits isn't a difference; a changed value is one, with its path; an
        // attribute only the actual one has is one unless it may add some.
        const auto expected = juce::parseXML (R"(<EDIT><TRACK id="1003" phaziteHeight="64" volume="0.6376281"/></EDIT>)");
        const auto noisy = juce::parseXML (R"(<EDIT><TRACK id="1003" phaziteHeight="64" volume="0.6376282"/></EDIT>)");
        const auto taller = juce::parseXML (R"(<EDIT><TRACK id="1003" phaziteHeight="96" volume="0.6376281"/></EDIT>)");
        const auto added = juce::parseXML (R"(<EDIT><TRACK id="1003" phaziteHeight="64" volume="0.6376281" mute="1"/></EDIT>)");
        REQUIRE (expected != nullptr);
        REQUIRE (noisy != nullptr);
        REQUIRE (taller != nullptr);
        REQUIRE (added != nullptr);

        CHECK (phazite::test::findDifferences (*expected, *noisy).isEmpty());

        const auto heightChange = phazite::test::findDifferences (*expected, *taller);
        REQUIRE (heightChange.size() == 1);
        CHECK (heightChange[0].startsWith ("EDIT/TRACK#1003@phaziteHeight: "));

        CHECK (phazite::test::findDifferences (*expected, *added).size() == 1);
        CHECK (phazite::test::findDifferences (*expected, *added, { .actualMayAdd = true }).isEmpty());
    }

    TEST_CASE ("Golden project: a new, empty project")
    {
        phazite::test::TemporaryFolder temp;
        auto project = createProject (temp.folder, "Empty");
        phazite::test::settle();

        // Without a MIDI clip, it opens without unsaved changes.
        saveReopenAndSaveAgain (std::move (project), "empty", true,
                                "A new, empty project (Project::create), saved, reopened and saved again: tests/GoldenProjectTests.cpp");
    }

    TEST_CASE ("Golden project: the starter project")
    {
        // As ProjectSession::createFirstProject makes Phazite's first project.
        phazite::test::TemporaryFolder temp;
        auto project = createProject (temp.folder, "Starter");
        phazite::addStarterContent (project->getEdit());
        REQUIRE (project->save().wasOk());
        phazite::test::settle();

        // Its MIDI clips give it unsaved changes when it is reopened (saveReopenAndSaveAgain() says why).
        saveReopenAndSaveAgain (std::move (project), "starter", false,
                                "The starter project (addStarterContent), saved, reopened and saved again: tests/GoldenProjectTests.cpp");
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

        // Its MIDI clips give it unsaved changes when it is reopened (saveReopenAndSaveAgain() says why).
        const auto saves = saveReopenAndSaveAgain (std::move (project), "representative", false,
                                                   "A project with one of each kind of track and every piece of Phazite's data, saved, "
                                                   "reopened and saved again: addRepresentativeContent in tests/GoldenProjectTests.cpp");

        // Nothing in it names the person who made it (Tracktion would write their full name: EngineSetup.h). A name
        // counts as a whole word in a value, so that a short one isn't found in a tag or a longer word, such as "root"
        // in phaziteKeyRoot or "tim" in TIMESIG.
        const auto values = getAllValues (*saves.rawFirst);

        for (const auto& name : { juce::SystemStats::getFullUserName().trim(), juce::SystemStats::getLogonName().trim() })
        {
            if (name.length() >= 3)
            {
                INFO (("The first save names the user: " + name).toStdString());
                CHECK_FALSE (values.containsWholeWordIgnoreCase (name));
            }
        }

        phazite::test::checkSameProject ("in the autosave", *saves.first, *autosave);
        phazite::test::checkSameProject ("saved as a copy", *saves.first, *copy);

        // Today's save is the 0.1.0 project that MigrationTests.cpp opens; it is kept once and never regenerated.
        phazite::test::freezeLegacyInput ("legacy/0.1.0-representative.phazite", *saves.rawSecond, "0.1.0");
    }
}
