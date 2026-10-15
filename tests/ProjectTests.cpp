// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/Sections.h"
#include "model/base/Migrations.h"
#include "model/base/Schema.h"
#include "model/notes/Notes.h"
#include "model/project/Project.h"
#include "model/project/StarterProject.h"
#include "model/transport/Playback.h"

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

// A folder in the temp directory that is deleted again at the end of the test.
struct TemporaryFolder
{
    TemporaryFolder() { folder.createDirectory(); }

    ~TemporaryFolder() { folder.deleteRecursively(); }

    const juce::File folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteProjectTests", {}, false);
};

std::unique_ptr<phazite::Project> createProject (const juce::File& parentFolder, const juce::String& name)
{
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

std::unique_ptr<phazite::Project> openProject (const juce::File& file, bool restoreAutosave = false)
{
    juce::String error;
    auto project = phazite::Project::open (phazite::test::getEngine(), file, restoreAutosave, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

juce::Array<juce::File> getBackups (const phazite::Project& project)
{
    auto files = project.getBackupsFolder().findChildFiles (juce::File::findFiles, false, "*.phazite");
    files.removeAllInstancesOf (phazite::Project::getAutosaveFile (project.getFile()));
    return files;
}
} // namespace

TEST_CASE ("A new project creates its folder, subfolders and file")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "My Song");

    CHECK (project->getFile() == temp.folder.getChildFile ("My Song").getChildFile ("My Song.phazite"));
    CHECK (project->getFile().existsAsFile());

    for (const auto& folder :
         { project->getAudioFolder(), project->getGeneratedFolder(), project->getRendersFolder(), project->getBackupsFolder() })
        CHECK (folder.isDirectory());

    CHECK_FALSE (project->hasUnsavedChanges());

    // In the current format, in memory and in the file.
    CHECK (phazite::getSchemaVersion (project->getEdit().state) == phazite::schema::currentVersion);
    const auto saved = phazite::test::readXml (project->getFile());
    REQUIRE (saved->getChildByName ("PHAZITE") != nullptr);
    CHECK (saved->getChildByName ("PHAZITE")->getIntAttribute ("schemaVersion") == phazite::schema::currentVersion);

    // A second project with the same name would overwrite the first, so it is refused.
    juce::String error;
    CHECK (phazite::Project::create (phazite::test::getEngine(), temp.folder, "My Song", error) == nullptr);
    CHECK (error.isNotEmpty());
}

TEST_CASE ("A project saves and reopens with Phazite's own data")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Round Trip");

    project->getEdit().ensureNumberOfAudioTracks (3);
    project->getPhaziteState().setProperty ("outline", "Electronic", nullptr);
    CHECK (project->hasUnsavedChanges());

    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    const auto file = project->getFile();
    const auto id = project->getEdit().getProjectItemRef();
    project = nullptr;

    auto reopened = openProject (file);
    CHECK (te::getAudioTracks (reopened->getEdit()).size() == 3);
    CHECK (reopened->getPhaziteState()["outline"].toString() == juce::String ("Electronic"));
    CHECK (reopened->getEdit().getProjectItemRef() == id); // so its temporary folder stays the same
}

TEST_CASE ("Saving keeps the newest backups")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Backups");
    CHECK (getBackups (*project).isEmpty()); // the first save had nothing to back up

    for (int i = 0; i < phazite::Project::numBackupsToKeep + 3; ++i)
    {
        project->getPhaziteState().setProperty ("version", i, nullptr);
        REQUIRE (project->save().wasOk());
    }

    CHECK (getBackups (*project).size() == phazite::Project::numBackupsToKeep);
}

TEST_CASE ("An autosave survives a crash and can be restored")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Crash");
    const auto file = project->getFile();
    const auto autosaveFile = phazite::Project::getAutosaveFile (file);

    project->getPhaziteState().setProperty ("take", 1, nullptr);
    REQUIRE (project->save().wasOk());

    project->getPhaziteState().setProperty ("take", 2, nullptr);
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::hasNewerAutosave (file));

    // Closing deletes the autosave; a crash wouldn't. Put back a copy, as if Phazite had crashed.
    juce::MemoryBlock leftOverAutosave;
    REQUIRE (autosaveFile.loadFileAsData (leftOverAutosave));
    project = nullptr;
    CHECK_FALSE (phazite::Project::hasNewerAutosave (file));
    REQUIRE (autosaveFile.replaceWithData (leftOverAutosave.getData(), leftOverAutosave.getSize()));
    CHECK (phazite::Project::hasNewerAutosave (file));

    // Opening without restoring shows the saved version.
    CHECK ((int) openProject (file)->getPhaziteState()["take"] == 1);
    REQUIRE (autosaveFile.replaceWithData (leftOverAutosave.getData(), leftOverAutosave.getSize()));

    // Restoring brings the autosaved change back, as an unsaved change to the same project file.
    auto restored = openProject (file, true);
    CHECK ((int) restored->getPhaziteState()["take"] == 2);
    CHECK (restored->hasUnsavedChanges());
    CHECK (restored->getFile() == file);

    REQUIRE (restored->save().wasOk());
    CHECK_FALSE (phazite::Project::hasNewerAutosave (file));
    restored = nullptr;

    CHECK ((int) openProject (file)->getPhaziteState()["take"] == 2);
}

TEST_CASE ("Autosave writes nothing when nothing changed")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Unchanged");

    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (project->getFile()).exists());
}

TEST_CASE ("Save As makes a copy with the unsaved changes, and leaves the original file alone")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Original");
    REQUIRE (project->getAudioFolder().getChildFile ("kick.wav").replaceWithText ("audio, as far as this test cares"));
    project->getPhaziteState().setProperty ("outline", "Pop", nullptr); // not saved yet

    juce::String error;
    const auto copyFile = project->saveCopyAs (temp.folder, "Copy", error);
    INFO (error.toStdString());
    REQUIRE (copyFile.existsAsFile());
    CHECK (copyFile == temp.folder.getChildFile ("Copy").getChildFile ("Copy.phazite"));
    CHECK (copyFile.getSiblingFile ("Audio").getChildFile ("kick.wav").existsAsFile());
    CHECK (copyFile.getSiblingFile ("Backups").isDirectory());

    // The copy has the change. The original's file doesn't, and the project still has it unsaved.
    CHECK (project->hasUnsavedChanges());
    const auto copy = openProject (copyFile);
    CHECK (copy->getPhaziteState()["outline"].toString() == juce::String ("Pop"));
    CHECK (openProject (project->getFile())->getPhaziteState()["outline"].toString().isEmpty());

    // The copy is a project of its own, with an ID of its own.
    CHECK (copy->getEdit().getProjectItemRef().isProjectItemID());
    CHECK (copy->getEdit().getProjectItemRef() != project->getEdit().getProjectItemRef());

    // A name that is taken is refused.
    error.clear();
    CHECK (project->saveCopyAs (temp.folder, "Copy", error) == juce::File());
    CHECK (error.isNotEmpty());
}

TEST_CASE ("A project opens with the playhead at the song's start, where playback starts")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Start");
    phazite::movePlayhead (project->getEdit(), te::TimePosition::fromSeconds (6.0));
    CHECK_FALSE (project->hasUnsavedChanges()); // where playback starts changes nothing in the song

    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    auto reopened = openProject (file);
    CHECK (reopened->getEdit().getTransport().getPosition().inSeconds() == doctest::Approx (0.0));
    CHECK (phazite::getPlayStart (reopened->getEdit()).inSeconds() == doctest::Approx (0.0));
}

TEST_CASE ("A new project opens from its first save with what it starts with, without undo steps or unsaved changes")
{
    TemporaryFolder temp;
    juce::String error;
    std::unique_ptr<phazite::Project> project;

    SUBCASE ("The starter song, as the first start makes it")
    {
        project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "First", error, phazite::addStarterContent);
        INFO (error.toStdString());
        REQUIRE (project != nullptr);
        const auto tracks = te::getAudioTracks (project->getEdit());
        REQUIRE (tracks.size() == 1);
        CHECK (tracks[0]->getName() == juce::String ("Synth"));
        CHECK (tracks[0]->getClips().size() == 1);
    }

    SUBCASE ("An outline's sections, as New Project lays them out")
    {
        project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Outlined", error,
                                            [] (te::Edit& e) { phazite::applyOutline (e, *phazite::findOutline ("pop")); });
        INFO (error.toStdString());
        REQUIRE (project != nullptr);
        CHECK (phazite::getAppliedOutline (project->getEdit()) == juce::String ("pop"));
        CHECK_FALSE (phazite::getSections (project->getEdit()).isEmpty());
    }

    // It is opened from its save: Tracktion put what was added in order while loading, and nothing of it is an undo step
    // or, once Tracktion's updates arrive, an unsaved change.
    CHECK (project->getFile().existsAsFile());
    CHECK_FALSE (project->getEdit().getUndoManager().canUndo());
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK_FALSE (project->getEdit().getUndoManager().canUndo());
}

TEST_CASE ("A project with MIDI clips and an alias opens without unsaved changes")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Notes");
    auto* track = te::getAudioTracks (project->getEdit())[0];
    REQUIRE (track != nullptr);
    auto* clip = phazite::insertMidiClip (*track, { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (clip != nullptr);
    REQUIRE (phazite::addNote (*clip, 60, beats (0.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
    REQUIRE (phazite::addNote (*clip, 64, beats (2.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
    const auto aliases = phazite::copyClips ({ clip }, true);
    REQUIRE (aliases.size() == 1);
    phazite::moveClip (*aliases[0], beats (4.0), *track);

    // As the app's message loop does between edits: Tracktion sorts the track's clips, and the alias copies the notes.
    phazite::test::settle();
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    // Once Tracktion's updates after loading arrive (each MIDI clip notifies its mute switch, and the alias copies its
    // notes again), nothing has changed.
    project = openProject (file);
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (file).exists());
}

TEST_CASE ("Only a change to what the project holds is an unsaved change, made with an undo step or not")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Changes");

    {
        auto* track = te::getAudioTracks (project->getEdit())[0];
        REQUIRE (track != nullptr);
        auto* clip = phazite::insertMidiClip (*track, { beats (0.0), beats (4.0) }, nullptr);
        REQUIRE (clip != nullptr);
        REQUIRE (phazite::addNote (*clip, 60, beats (0.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
        REQUIRE (phazite::addNote (*clip, 64, beats (2.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
        const auto aliases = phazite::copyClips ({ clip }, true);
        REQUIRE (aliases.size() == 1);
        phazite::moveClip (*aliases[0], beats (4.0), *track);
        phazite::setTrackKey (*track, phazite::Key { 2, te::Scale::minor });
        phazite::test::settle();
        REQUIRE (project->save().wasOk());
    }

    const auto file = project->getFile();
    project = nullptr;
    project = openProject (file);
    phazite::test::settle();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    auto& edit = project->getEdit();
    auto* track = te::getAudioTracks (edit)[0];
    REQUIRE (track != nullptr);
    auto* clip = dynamic_cast<te::MidiClip*> (track->getClips().getFirst()); // the original, at beat 0
    REQUIRE (clip != nullptr);

    // Notifications that change nothing.
    clip->state.sendPropertyChangeMessage (te::IDs::mute);
    track->state.sendPropertyChangeMessage (te::IDs::name);
    CHECK_FALSE (project->hasUnsavedChanges());

    // The same value written as another type (the file holds text).
    REQUIRE (track->state[phazite::schema::phaziteKeyRoot].isString());
    track->state.setProperty (phazite::schema::phaziteKeyRoot, 2, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());

    // A note taken out and put back as it was.
    auto sequence = clip->getSequence().state;
    const auto note = sequence.getChild (0);
    sequence.removeChild (note, nullptr);
    sequence.addChild (note.createCopy(), 0, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());

    // Where the playhead is.
    phazite::movePlayhead (edit, te::TimePosition::fromSeconds (2.0));
    track->state.sendPropertyChangeMessage (te::IDs::name);
    CHECK_FALSE (project->hasUnsavedChanges());

    // A change taken back before anything asked.
    track->state.setProperty (phazite::schema::phaziteKeyRoot, 5, nullptr);
    track->state.setProperty (phazite::schema::phaziteKeyRoot, 2, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (file).exists());

    // Real changes count, made without an undo step or with one; saving clears them.
    project->getPhaziteState().setProperty (phazite::schema::outline, "pop", nullptr);
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::getAutosaveFile (file).existsAsFile());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    REQUIRE (clip->getSequence().addNote (67, beats (3.0), te::BeatDuration::fromBeats (0.5), 100, 0, nullptr) != nullptr);
    CHECK (project->hasUnsavedChanges());
    phazite::test::settle(); // the alias copies the note
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // With an undo step, as the editors make changes: Tracktion's listener for undo steps then marks the Edit changed
    // too, with its flag and its time stamp, lastSignificantChange.
    REQUIRE (phazite::addNote (*clip, 69, beats (3.5), te::BeatDuration::fromBeats (0.25), 100) != nullptr);
    CHECK (project->hasUnsavedChanges());
    phazite::test::settle();
    CHECK (edit.hasChangedSinceSaved());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // A change Tracktion notices outside the project's values, such as a plug-in's own settings, reaches Phazite only as
    // that time stamp once Tracktion's flag is reset (as it is 200 ms after loading): it counts, and autosave writes it.
    // (The stamp is in milliseconds; the settle() above makes this one new.)
    edit.markAsChanged();
    edit.resetChangedStatus();
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::getAutosaveFile (file).existsAsFile());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    track->state.setProperty (phazite::schema::phaziteKeyRoot, 3, nullptr);
    CHECK (project->hasUnsavedChanges());
}
